/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * USB3HDCAP camera extension (CoreMediaIO system extension). One device, "USB3HDCAP", with one
 * source stream. Frames come from hdcapd through HDCapFrameSource, scaled (letterboxed) to the
 * format the client selected; black filler frames keep the stream running without a signal.
 * The device is linked to the HAL plug-in's audio device so apps can pair picture and sound.
 */
#import "HDCapFrameSource.h"
#import <CoreMediaIO/CMIOExtension.h>
#import <CoreAudio/AudioHardware.h>
#import <os/log.h>

#define HAL_DEVICE_UID @"org.open-usb3hdcap.hal.device"

static os_log_t g_log;

typedef struct { int w, h, fps; } CamMode;
static const CamMode kModes[] = {
    { 1920, 1080, 60 }, { 1920, 1080, 30 }, { 1280, 720, 60 }, { 1280, 720, 30 }, { 640, 480, 60 }, { 640, 480, 30 },
};
#define MODE_COUNT (sizeof kModes / sizeof kModes[0])

@class CamStreamSource;

@interface CamDeviceSource : NSObject <CMIOExtensionDeviceSource>
@property (nonatomic, strong) CMIOExtensionDevice *device;
@property (nonatomic, strong) CamStreamSource *streamSource;
- (instancetype)initWithLocalizedName:(NSString *)name;
@end

@interface CamStreamSource : NSObject <CMIOExtensionStreamSource>
@property (nonatomic, strong) CMIOExtensionStream *stream;
@property (nonatomic, weak) CamDeviceSource *deviceSource;
@property (nonatomic) NSUInteger activeFormatIndex;
@end

@interface CamProviderSource : NSObject <CMIOExtensionProviderSource>
@property (nonatomic, strong) CMIOExtensionProvider *provider;
@property (nonatomic, strong) CamDeviceSource *deviceSource;
@end

/* ---------- stream ---------- */

@implementation CamStreamSource {
    NSArray<CMIOExtensionStreamFormat *> *_formats;
    HDCapFrameSource *_source;
    NSUInteger _streamingCount;
    BOOL _lastWasSynthetic;
}

- (instancetype)initWithLocalizedName:(NSString *)name streamID:(NSUUID *)sid
{
    if ((self = [super init])) {
        NSMutableArray *formats = [NSMutableArray array];
        for (size_t i = 0; i < MODE_COUNT; i++) {
            CMVideoFormatDescriptionRef fd = NULL;
            CMVideoFormatDescriptionCreate(NULL, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kModes[i].w, kModes[i].h, NULL, &fd);
            CMTime dur = CMTimeMake(1, kModes[i].fps);
            [formats addObject:[CMIOExtensionStreamFormat streamFormatWithFormatDescription:fd maxFrameDuration:dur minFrameDuration:dur validFrameDurations:nil]];
            CFRelease(fd);
        }
        _formats = formats;
        _activeFormatIndex = 2; /* 1280x720@60 */
        _stream = [CMIOExtensionStream streamWithLocalizedName:name streamID:sid direction:CMIOExtensionStreamDirectionSource
                                                    clockType:CMIOExtensionStreamClockTypeHostTime source:self];
        const CamMode *m = &kModes[_activeFormatIndex];
        _source = [[HDCapFrameSource alloc] initWithWidth:m->w height:m->h fps:m->fps];
        __weak CamStreamSource *weakSelf = self;
        _source.frameHandler = ^(CVPixelBufferRef buf, uint64_t hostNs, BOOL synthetic) { [weakSelf sendBuffer:buf hostNs:hostNs synthetic:synthetic]; };
    }
    return self;
}

- (void)sendBuffer:(CVPixelBufferRef)buf hostNs:(uint64_t)hostNs synthetic:(BOOL)synthetic
{
    CMVideoFormatDescriptionRef fd = NULL;
    if (CMVideoFormatDescriptionCreateForImageBuffer(NULL, buf, &fd) != noErr)
        return;
    const CamMode *m = &kModes[_activeFormatIndex];
    CMSampleTimingInfo timing = { CMTimeMake(1, m->fps), CMTimeMake((int64_t)hostNs, 1000000000), kCMTimeInvalid };
    CMSampleBufferRef sb = NULL;
    if (CMSampleBufferCreateReadyWithImageBuffer(NULL, buf, fd, &timing, &sb) == noErr) {
        CMIOExtensionStreamDiscontinuityFlags flags = synthetic != _lastWasSynthetic ? CMIOExtensionStreamDiscontinuityFlagTime
                                                                                    : CMIOExtensionStreamDiscontinuityFlagNone;
        [_stream sendSampleBuffer:sb discontinuity:flags hostTimeInNanoseconds:hostNs];
        CFRelease(sb);
    }
    _lastWasSynthetic = synthetic;
    CFRelease(fd);
}

- (NSArray<CMIOExtensionStreamFormat *> *)formats { return _formats; }

- (NSSet<CMIOExtensionProperty> *)availableProperties
{
    return [NSSet setWithObjects:CMIOExtensionPropertyStreamActiveFormatIndex, CMIOExtensionPropertyStreamFrameDuration, nil];
}

- (CMIOExtensionStreamProperties *)streamPropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties error:(NSError **)outError
{
    CMIOExtensionStreamProperties *p = [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];
    if ([properties containsObject:CMIOExtensionPropertyStreamActiveFormatIndex])
        p.activeFormatIndex = @(_activeFormatIndex);
    if ([properties containsObject:CMIOExtensionPropertyStreamFrameDuration]) {
        CMTime d = CMTimeMake(1, kModes[_activeFormatIndex].fps);
        p.frameDuration = CFBridgingRelease(CMTimeCopyAsDictionary(d, NULL));
    }
    return p;
}

- (BOOL)setStreamProperties:(CMIOExtensionStreamProperties *)streamProperties error:(NSError **)outError
{
    NSNumber *idx = streamProperties.activeFormatIndex;
    if (idx && idx.unsignedIntegerValue < MODE_COUNT && idx.unsignedIntegerValue != _activeFormatIndex) {
        _activeFormatIndex = idx.unsignedIntegerValue;
        const CamMode *m = &kModes[_activeFormatIndex];
        [_source setWidth:m->w height:m->h fps:m->fps];
        os_log(g_log, "format -> %dx%d@%d", m->w, m->h, m->fps);
    }
    return YES;
}

- (BOOL)authorizedToStartStreamForClient:(CMIOExtensionClient *)client { return YES; }

- (BOOL)startStreamAndReturnError:(NSError **)outError
{
    if (_streamingCount++ == 0) {
        os_log(g_log, "stream start");
        _lastWasSynthetic = NO;
        [_source start];
    }
    return YES;
}

- (BOOL)stopStreamAndReturnError:(NSError **)outError
{
    if (_streamingCount && --_streamingCount == 0) {
        os_log(g_log, "stream stop");
        [_source stop];
    }
    return YES;
}
@end

/* ---------- device ---------- */

@implementation CamDeviceSource
- (instancetype)initWithLocalizedName:(NSString *)name
{
    if ((self = [super init])) {
        /* Fixed IDs so apps remember the selected camera across restarts. */
        NSUUID *devID = [[NSUUID alloc] initWithUUIDString:@"5B1D3A2E-8C47-4E0B-9A61-2F3C8D7E4B10"];
        NSUUID *strID = [[NSUUID alloc] initWithUUIDString:@"9E6F0C41-37B2-4D58-B1A3-6C2D9F8E5A27"];
        _device = [CMIOExtensionDevice deviceWithLocalizedName:name deviceID:devID legacyDeviceID:@"org.open-usb3hdcap.camera" source:self];
        _streamSource = [[CamStreamSource alloc] initWithLocalizedName:@"USB3HDCAP Video" streamID:strID];
        _streamSource.deviceSource = self;
        NSError *err = nil;
        if (![_device addStream:_streamSource.stream error:&err])
            os_log_error(g_log, "addStream: %{public}@", err);
    }
    return self;
}

- (NSSet<CMIOExtensionProperty> *)availableProperties
{
    return [NSSet setWithObjects:CMIOExtensionPropertyDeviceTransportType, CMIOExtensionPropertyDeviceModel,
                                 CMIOExtensionPropertyDeviceLinkedCoreAudioDeviceUID, nil];
}

- (CMIOExtensionDeviceProperties *)devicePropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties error:(NSError **)outError
{
    CMIOExtensionDeviceProperties *p = [CMIOExtensionDeviceProperties devicePropertiesWithDictionary:@{}];
    if ([properties containsObject:CMIOExtensionPropertyDeviceTransportType])
        p.transportType = @(kAudioDeviceTransportTypeUSB);
    if ([properties containsObject:CMIOExtensionPropertyDeviceModel])
        p.model = @"USB3HDCAP";
    if ([properties containsObject:CMIOExtensionPropertyDeviceLinkedCoreAudioDeviceUID])
        p.linkedCoreAudioDeviceUID = HAL_DEVICE_UID;
    return p;
}

- (BOOL)setDeviceProperties:(CMIOExtensionDeviceProperties *)deviceProperties error:(NSError **)outError { return YES; }
@end

/* ---------- provider ---------- */

@implementation CamProviderSource
- (instancetype)initWithClientQueue:(dispatch_queue_t)queue
{
    if ((self = [super init])) {
        _provider = [CMIOExtensionProvider providerWithSource:self clientQueue:queue];
        _deviceSource = [[CamDeviceSource alloc] initWithLocalizedName:@"USB3HDCAP"];
        NSError *err = nil;
        if (![_provider addDevice:_deviceSource.device error:&err])
            os_log_error(g_log, "addDevice: %{public}@", err);
    }
    return self;
}
- (BOOL)connectClient:(CMIOExtensionClient *)client error:(NSError **)outError { return YES; }
- (void)disconnectClient:(CMIOExtensionClient *)client {}
- (NSSet<CMIOExtensionProperty> *)availableProperties
{
    return [NSSet setWithObjects:CMIOExtensionPropertyProviderManufacturer, nil];
}
- (CMIOExtensionProviderProperties *)providerPropertiesForProperties:(NSSet<CMIOExtensionProperty> *)properties error:(NSError **)outError
{
    CMIOExtensionProviderProperties *p = [CMIOExtensionProviderProperties providerPropertiesWithDictionary:@{}];
    if ([properties containsObject:CMIOExtensionPropertyProviderManufacturer])
        p.manufacturer = @"open-usb3hdcap";
    return p;
}
- (BOOL)setProviderProperties:(CMIOExtensionProviderProperties *)providerProperties error:(NSError **)outError { return YES; }
@end

int main(void)
{
    @autoreleasepool {
        g_log = os_log_create("org.open-usb3hdcap.camera", "extension");
        CamProviderSource *ps = [[CamProviderSource alloc] initWithClientQueue:nil];
        [CMIOExtensionProvider startServiceWithProvider:ps.provider];
        os_log(g_log, "camera extension started");
        CFRunLoopRun();
    }
    return 0;
}
