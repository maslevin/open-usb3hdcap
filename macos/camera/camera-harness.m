/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * camera-harness: drive the camera extension's provider/device/stream classes in-process (no
 * system extension activation needed). CMIOExtensionStream's sendSampleBuffer is intercepted, so
 * every frame the extension would hand to CoreMediaIO is checked: size vs active format,
 * timestamps (monotonic, close to now), discontinuity flags, real vs filler. Walks through
 * several formats and a stop/restart; saves one frame per format as PPM into OUTDIR.
 *   camera-harness [SECONDS_PER_STEP] [OUTDIR]
 */
#import "CameraExtension.h"
#import <CoreMediaIO/CMIOExtension.h>
#import <objc/runtime.h>
#include "hdcap.h"
#include <stdio.h>

static struct {
    int frames, wrong_size, non_monotonic, discont, black;
    uint64_t last_host, max_age_ns;
    int expect_w, expect_h, saved;
    const char *outdir;
} g;

static void save_ppm(CVPixelBufferRef b, const char *path)
{
    size_t bw = CVPixelBufferGetWidth(b), bh = CVPixelBufferGetHeight(b);
    uint8_t *yuyv = malloc(bw * bh * 2), *rgb = malloc(bw * bh * 3);
    CVPixelBufferLockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
    const uint8_t *Y = CVPixelBufferGetBaseAddressOfPlane(b, 0), *UV = CVPixelBufferGetBaseAddressOfPlane(b, 1);
    size_t ys = CVPixelBufferGetBytesPerRowOfPlane(b, 0), uvs = CVPixelBufferGetBytesPerRowOfPlane(b, 1);
    for (size_t y = 0; y < bh; y++)
        for (size_t x = 0; x < bw; x += 2) {
            uint8_t *o = yuyv + (y * bw + x) * 2;
            const uint8_t *uv = UV + (y / 2) * uvs + x;
            o[0] = Y[y * ys + x]; o[1] = uv[0]; o[2] = Y[y * ys + x + 1]; o[3] = uv[1];
        }
    CVPixelBufferUnlockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
    hdcap_yuyv_to_rgb24(yuyv, rgb, (int)bw, (int)bh);
    FILE *f = fopen(path, "wb");
    if (f) { fprintf(f, "P6\n%zu %zu\n255\n", bw, bh); fwrite(rgb, 1, bw * bh * 3, f); fclose(f); }
    free(yuyv); free(rgb);
}

static void fake_send(id self, SEL _cmd, CMSampleBufferRef sb, CMIOExtensionStreamDiscontinuityFlags flags, uint64_t hostNs)
{
    (void)self; (void)_cmd;
    CVPixelBufferRef b = CMSampleBufferGetImageBuffer(sb);
    int w = (int)CVPixelBufferGetWidth(b), h = (int)CVPixelBufferGetHeight(b);
    g.frames++;
    if (w != g.expect_w || h != g.expect_h) g.wrong_size++;
    if (g.last_host && hostNs <= g.last_host) g.non_monotonic++;
    if (flags) g.discont++;
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
    if ((uint64_t)CMTimeConvertScale(pts, 1000000000, kCMTimeRoundingMethod_Default).value != hostNs) g.non_monotonic += 1000;
    uint64_t age = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - hostNs;
    if (age > g.max_age_ns) g.max_age_ns = age;
    if (age > 50000000) printf("    late frame #%d: %.1f ms old, %dx%d\n", g.frames, age / 1e6, w, h);
    g.last_host = hostNs;
    /* black filler: centre luma == 16 */
    CVPixelBufferLockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
    const uint8_t *Y = CVPixelBufferGetBaseAddressOfPlane(b, 0);
    if (Y[(h / 2) * CVPixelBufferGetBytesPerRowOfPlane(b, 0) + w / 2] == 16 && Y[(h / 3) * CVPixelBufferGetBytesPerRowOfPlane(b, 0) + w / 3] == 16) g.black++;
    CVPixelBufferUnlockBaseAddress(b, kCVPixelBufferLock_ReadOnly);
    if (g.outdir && !g.saved && g.frames > 20) {
        char path[1024];
        snprintf(path, sizeof path, "%s/camera-%dx%d.ppm", g.outdir, w, h);
        save_ppm(b, path);
        g.saved = 1;
    }
}

static int step(CamStreamSource *ss, NSUInteger fmt, int seconds)
{
    NSError *err = nil;
    CMIOExtensionStreamProperties *p = [CMIOExtensionStreamProperties streamPropertiesWithDictionary:@{}];
    p.activeFormatIndex = @(fmt);
    [ss setStreamProperties:p error:&err];
    memset(&g, 0, offsetof(typeof(g), expect_w));
    g.expect_w = kCamModes[fmt].w; g.expect_h = kCamModes[fmt].h; g.saved = 0;
    sleep((unsigned)seconds);
    CMIOExtensionStreamProperties *rp = [ss streamPropertiesForProperties:[NSSet setWithObjects:CMIOExtensionPropertyStreamActiveFormatIndex,
                                                                             CMIOExtensionPropertyStreamFrameDuration, nil] error:&err];
    CMTime dur = CMTimeMakeFromDictionary((__bridge CFDictionaryRef)rp.frameDuration);
    double fps = (double)g.frames / seconds;
    int ok = g.wrong_size == 0 && g.non_monotonic == 0 && fps > kCamModes[fmt].fps * 0.45;
    printf("  format %lu %4dx%-4d@%d: %5.1f frames/s (%d black)  wrong size %d  non-monotonic %d  discont %d  max age %.1f ms  "
           "reported idx %s dur %lld/%d  %s\n", (unsigned long)fmt, kCamModes[fmt].w, kCamModes[fmt].h, kCamModes[fmt].fps, fps, g.black,
           g.wrong_size, g.non_monotonic, g.discont, g.max_age_ns / 1e6, rp.activeFormatIndex.description.UTF8String, dur.value, dur.timescale, ok ? "OK" : "FAIL");
    return ok;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        int secs = argc > 1 ? atoi(argv[1]) : 3;
        const char *outdir = argc > 2 ? argv[2] : NULL;
        Method m = class_getInstanceMethod([CMIOExtensionStream class], @selector(sendSampleBuffer:discontinuity:hostTimeInNanoseconds:));
        method_setImplementation(m, (IMP)fake_send);

        CamProviderSource *ps = [[CamProviderSource alloc] initWithClientQueue:nil];
        NSError *err = nil;
        CMIOExtensionProviderProperties *pp = [ps providerPropertiesForProperties:ps.availableProperties error:&err];
        printf("provider: manufacturer %s, %lu device(s)\n", pp.manufacturer.UTF8String, (unsigned long)ps.provider.devices.count);
        CamDeviceSource *ds = ps.deviceSource;
        CMIOExtensionDeviceProperties *dp = [ds devicePropertiesForProperties:ds.availableProperties error:&err];
        printf("device: %s  model %s  transport %s  linked audio %s  streams %lu\n", ds.device.localizedName.UTF8String, dp.model.UTF8String,
               dp.transportType.description.UTF8String, dp.linkedCoreAudioDeviceUID.UTF8String, (unsigned long)ds.device.streams.count);
        CamStreamSource *ss = ds.streamSource;
        printf("stream: %s, %lu formats\n", ss.stream.localizedName.UTF8String, (unsigned long)ss.formats.count);
        for (CMIOExtensionStreamFormat *f in ss.formats) {
            CMVideoDimensions d = CMVideoFormatDescriptionGetDimensions(f.formatDescription);
            FourCharCode pf = CMFormatDescriptionGetMediaSubType(f.formatDescription);
            printf("  %dx%d %c%c%c%c  max dur %lld/%d\n", d.width, d.height, pf >> 24, (pf >> 16) & 255, (pf >> 8) & 255, pf & 255,
                   f.maxFrameDuration.value, f.maxFrameDuration.timescale);
        }

        int ok = 1;
        g.outdir = outdir;
        [ss startStreamAndReturnError:&err];
        g.outdir = outdir;
        NSUInteger order[] = { 2, 0, 1, 4 };
        for (size_t i = 0; i < 4; i++) {
            const char *od = g.outdir;
            ok &= step(ss, order[i], secs);
            g.outdir = od;
        }
        [ss stopStreamAndReturnError:&err];
        int before = g.frames;
        sleep(1);
        printf("  after stop: %d frames in 1 s  %s\n", g.frames - before, g.frames == before ? "OK" : "FAIL");
        ok &= g.frames == before;
        printf("restart:\n");
        [ss startStreamAndReturnError:&err];
        ok &= step(ss, 2, secs);
        [ss stopStreamAndReturnError:&err];
        printf("%s\n", ok ? "ALL OK" : "FAILURES");
        return ok ? 0 : 1;
    }
}
