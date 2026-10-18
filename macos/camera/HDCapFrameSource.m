/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
#import "HDCapFrameSource.h"
#include "../common/hdcapd_protocol.h"

#include <xpc/xpc.h> /* before IOSurface: enables IOSurfaceLookupFromXPCObject */
#import <IOSurface/IOSurfaceAPI.h>
#import <VideoToolbox/VideoToolbox.h>
#include <time.h>

#define NO_SIGNAL_AFTER_NS 250000000ull   /* emit filler frames when no real frame for this long */

static uint64_t host_now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

@implementation HDCapFrameSource {
    dispatch_queue_t _q;
    xpc_connection_t _conn;
    dispatch_source_t _timer;
    VTPixelTransferSessionRef _xfer;
    CVPixelBufferPoolRef _pool;
    CVPixelBufferRef _black;
    uint64_t _lastRealNs;
    BOOL _running;
}

- (instancetype)initWithWidth:(int)width height:(int)height fps:(int)fps
{
    if ((self = [super init])) {
        dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        _q = dispatch_queue_create("org.open-usb3hdcap.framesource", attr);
        VTPixelTransferSessionCreate(NULL, &_xfer);
        VTSessionSetProperty(_xfer, kVTPixelTransferPropertyKey_ScalingMode, kVTScalingMode_Letterbox);
        VTSessionSetProperty(_xfer, kVTPixelTransferPropertyKey_DestinationYCbCrMatrix, kCVImageBufferYCbCrMatrix_ITU_R_709_2);
        [self configureWidth:width height:height fps:fps];
    }
    return self;
}

- (void)dealloc
{
    [self stop];
    if (_xfer) { VTPixelTransferSessionInvalidate(_xfer); CFRelease(_xfer); }
    if (_pool) CVPixelBufferPoolRelease(_pool);
    if (_black) CVPixelBufferRelease(_black);
}

/* Output: NV12 video range ('420v'), the format every camera consumer handles. */
- (void)configureWidth:(int)width height:(int)height fps:(int)fps
{
    _width = width; _height = height; _fps = fps > 0 ? fps : 30;
    if (_pool) CVPixelBufferPoolRelease(_pool);
    if (_black) CVPixelBufferRelease(_black);
    _pool = NULL; _black = NULL;
    NSDictionary *attrs = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (id)kCVPixelBufferWidthKey: @(width),
        (id)kCVPixelBufferHeightKey: @(height),
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
    };
    CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)attrs, &_pool);
    if (_pool && CVPixelBufferPoolCreatePixelBuffer(NULL, _pool, &_black) == kCVReturnSuccess) {
        CVPixelBufferLockBaseAddress(_black, 0);
        memset(CVPixelBufferGetBaseAddressOfPlane(_black, 0), 16, CVPixelBufferGetBytesPerRowOfPlane(_black, 0) * (size_t)height);
        memset(CVPixelBufferGetBaseAddressOfPlane(_black, 1), 128, CVPixelBufferGetBytesPerRowOfPlane(_black, 1) * (size_t)(height / 2));
        CVPixelBufferUnlockBaseAddress(_black, 0);
        [self tagBuffer:_black];
    }
}

- (void)tagBuffer:(CVPixelBufferRef)b
{
    CVBufferSetAttachment(b, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(b, kCVImageBufferColorPrimariesKey, kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(b, kCVImageBufferTransferFunctionKey, kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
}

- (void)setWidth:(int)width height:(int)height fps:(int)fps
{
    dispatch_sync(_q, ^{
        [self configureWidth:width height:height fps:fps];
        if (self->_running) [self startTimer];
    });
}

- (void)start
{
    dispatch_async(_q, ^{
        if (self->_running) return;
        self->_running = YES;
        self->_lastRealNs = 0;
        [self connect];
        [self startTimer];
    });
}

- (void)stop
{
    dispatch_sync(_q, ^{
        self->_running = NO;
        if (self->_timer) { dispatch_source_cancel(self->_timer); self->_timer = nil; }
        if (self->_conn) { xpc_connection_cancel(self->_conn); self->_conn = nil; }
    });
}

- (void)startTimer
{
    if (_timer) dispatch_source_cancel(_timer);
    _timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _q);
    uint64_t period = 1000000000ull / (uint64_t)_fps;
    dispatch_source_set_timer(_timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)period), period, period / 10);
    __weak HDCapFrameSource *weakSelf = self;
    dispatch_source_set_event_handler(_timer, ^{ [weakSelf fillerTick]; });
    dispatch_resume(_timer);
}

- (void)fillerTick
{
    uint64_t now = host_now_ns();
    if (_black && _frameHandler && now - _lastRealNs > NO_SIGNAL_AFTER_NS)
        _frameHandler(_black, now, YES);
}

- (void)connect
{
    xpc_connection_t c = xpc_connection_create_mach_service(HDCAPD_SERVICE_NAME, _q, 0);
    __weak HDCapFrameSource *weakSelf = self;
    xpc_connection_set_event_handler(c, ^(xpc_object_t ev) {
        HDCapFrameSource *s = weakSelf;
        if (!s) return;
        if (xpc_get_type(ev) == XPC_TYPE_ERROR) {
            if (s->_conn == c) s->_conn = nil;
            /* daemon restarted or not loaded: keep retrying while running (filler frames continue) */
            if (s->_running)
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), s->_q, ^{
                    if (s->_running && !s->_conn) [s connect];
                });
            return;
        }
        [s handleMessage:ev];
    });
    xpc_connection_resume(c);
    _conn = c;
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(m, "cmd", "hello");
    xpc_dictionary_set_string(m, "client", "camera");
    xpc_dictionary_set_bool(m, "video", true);
    xpc_dictionary_set_bool(m, "audio", false);
    xpc_connection_send_message_with_reply(c, m, _q, ^(xpc_object_t r) {
        HDCapFrameSource *s = weakSelf;
        if (s && xpc_get_type(r) == XPC_TYPE_DICTIONARY) [s reportStatus:r];
    });
}

- (void)reportStatus:(xpc_object_t)d
{
    if (!_statusHandler) return;
    NSMutableDictionary *st = [NSMutableDictionary dictionary];
    const char *name = xpc_dictionary_get_string(d, "state_name"), *detail = xpc_dictionary_get_string(d, "detail");
    if (name) st[@"state"] = @(name);
    if (detail) st[@"detail"] = @(detail);
    st[@"width"] = @(xpc_dictionary_get_int64(d, "width"));
    st[@"height"] = @(xpc_dictionary_get_int64(d, "height"));
    st[@"fps"] = @(xpc_dictionary_get_double(d, "fps"));
    _statusHandler(st);
}

- (void)handleMessage:(xpc_object_t)ev
{
    if (xpc_get_type(ev) != XPC_TYPE_DICTIONARY) return;
    const char *type = xpc_dictionary_get_string(ev, "type");
    if (type && !strcmp(type, "status")) { [self reportStatus:ev]; return; }
    if (!type || strcmp(type, "frame") || !_frameHandler || !_pool) return;

    IOSurfaceRef surf = IOSurfaceLookupFromXPCObject(xpc_dictionary_get_value(ev, "surface"));
    if (!surf) return;
    CVPixelBufferRef src = NULL, dst = NULL;
    CVPixelBufferCreateWithIOSurface(NULL, surf, NULL, &src);
    CFRelease(surf);
    if (!src) return;
    /* hdcapd output is BT.709/601 per input; tag as 709 (HD inputs) for the converter. */
    CVBufferSetAttachment(src, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    if (CVPixelBufferPoolCreatePixelBuffer(NULL, _pool, &dst) == kCVReturnSuccess &&
        VTPixelTransferSessionTransferImage(_xfer, src, dst) == noErr) {
        [self tagBuffer:dst];
        uint64_t t = xpc_dictionary_get_uint64(ev, "time_ns");
        _lastRealNs = host_now_ns();
        _frameHandler(dst, t ? t : _lastRealNs, NO);
    }
    if (dst) CVPixelBufferRelease(dst);
    CVPixelBufferRelease(src);
}
@end
