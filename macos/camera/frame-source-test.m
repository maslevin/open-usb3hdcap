/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * frame-source-test: run HDCapFrameSource (the camera extension's pipeline) outside the extension.
 *   frame-source-test [SECONDS] [WIDTHxHEIGHT] [OUT.ppm]
 * Reports real/filler frames per second and saves one converted NV12 frame as PPM.
 */
#import "HDCapFrameSource.h"
#include "hdcap.h"
#include <stdio.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        int seconds = argc > 1 ? atoi(argv[1]) : 5, w = 1920, h = 1080;
        if (argc > 2) sscanf(argv[2], "%dx%d", &w, &h);
        const char *ppm = argc > 3 ? argv[3] : NULL;
        __block int real = 0, filler = 0, saved = 0;
        __block uint64_t maxLagNs = 0;
        HDCapFrameSource *src = [[HDCapFrameSource alloc] initWithWidth:w height:h fps:60];
        src.statusHandler = ^(NSDictionary *st) { printf("  [status] %s\n", st.description.UTF8String); };
        src.frameHandler = ^(CVPixelBufferRef b, uint64_t t, BOOL synthetic) {
            if (synthetic) { filler++; return; }
            real++;
            uint64_t lag = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t;
            if (lag > maxLagNs) maxLagNs = lag;
            if (!ppm || saved || real < 30) return;
            /* NV12 -> YUYV -> RGB via libhdcap's converter (good enough for a check image) */
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
            FILE *f = fopen(ppm, "wb");
            if (f) { fprintf(f, "P6\n%zu %zu\n255\n", bw, bh); fwrite(rgb, 1, bw * bh * 3, f); fclose(f); }
            free(yuyv); free(rgb);
            printf("  saved %s (%zux%zu NV12)\n", ppm, bw, bh);
            saved = 1;
        };
        [src start];
        for (int t = 1; t <= seconds; t++) {
            sleep(1);
            printf("%3ds  real %3d/s  filler %3d/s  max lag %.1f ms\n", t, real, filler, maxLagNs / 1e6);
            real = filler = 0; maxLagNs = 0;
        }
        [src stop];
    }
    return 0;
}
