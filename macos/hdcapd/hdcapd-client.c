/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * hdcapd-client: test client for hdcapd.
 *   hdcapd-client status
 *   hdcapd-client watch [SECONDS] [OUT.ppm]     subscribe to video + audio, report rates, save a frame
 *   hdcapd-client drift [SECONDS]               measure the card's audio clock against the host clock
 *   hdcapd-client set INPUT [ANALOG_MODE H_OFF V_OFF TV_STD]   (INPUT: hdmi dvi vga component composite s-video)
 */
#include "hdcap.h"
#include "../common/hdcapd_protocol.h"

#include <xpc/xpc.h> /* before IOSurface: enables IOSurfaceCreateXPCObject */
#include <IOSurface/IOSurfaceAPI.h>
#include <dispatch/dispatch.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void print_status(xpc_object_t d)
{
    printf("state %-10s input %-9s %4lldx%-4lld %s fps %5.2f  link %lld Mbit/s  signal %lld lines %.3f kHz %.2f Hz  [%s]\n",
           xpc_dictionary_get_string(d, "state_name") ?: "?", xpc_dictionary_get_string(d, "input_name") ?: "?",
           xpc_dictionary_get_int64(d, "width"), xpc_dictionary_get_int64(d, "height"),
           xpc_dictionary_get_bool(d, "interlaced") ? "i" : "p", xpc_dictionary_get_double(d, "fps"),
           xpc_dictionary_get_int64(d, "link_mbps"), xpc_dictionary_get_int64(d, "vtotal"),
           xpc_dictionary_get_double(d, "hfreq_hz") / 1000, xpc_dictionary_get_double(d, "vfreq_hz"),
           xpc_dictionary_get_string(d, "detail") ?: "");
}

static xpc_connection_t connect_daemon(dispatch_queue_t q, void (^handler)(xpc_object_t))
{
    xpc_connection_t c = xpc_connection_create_mach_service(HDCAPD_SERVICE_NAME, q, 0);
    xpc_connection_set_event_handler(c, ^(xpc_object_t ev) {
        if (xpc_get_type(ev) == XPC_TYPE_ERROR) {
            fprintf(stderr, "connection error: %s\n", xpc_dictionary_get_string(ev, XPC_ERROR_KEY_DESCRIPTION));
            exit(1);
        }
        if (handler)
            handler(ev);
    });
    xpc_connection_resume(c);
    return c;
}

static xpc_object_t request(xpc_connection_t c, xpc_object_t msg)
{
    xpc_object_t r = xpc_connection_send_message_with_reply_sync(c, msg);
    if (xpc_get_type(r) != XPC_TYPE_DICTIONARY) {
        fprintf(stderr, "no reply from %s (is the daemon loaded?)\n", HDCAPD_SERVICE_NAME);
        exit(1);
    }
    return r;
}

static int parse_input(const char *s)
{
    for (int k = 0; k < HDCAP_INPUT_COUNT; k++)
        if (!strcasecmp(s, hdcap_input_name((hdcap_input)k)) || (k == HDCAP_INPUT_SVIDEO && !strcasecmp(s, "svideo")))
            return k;
    return -1;
}

int main(int argc, char **argv)
{
    const char *cmd = argc > 1 ? argv[1] : "status";
    dispatch_queue_t q = dispatch_queue_create("client", DISPATCH_QUEUE_SERIAL);

    if (!strcmp(cmd, "status")) {
        xpc_connection_t c = connect_daemon(q, NULL);
        xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(m, "cmd", "get-status");
        print_status(request(c, m));
        return 0;
    }
    if (!strcmp(cmd, "set") && argc > 2) {
        int in = parse_input(argv[2]);
        if (in < 0) {
            fprintf(stderr, "unknown input %s\n", argv[2]);
            return 2;
        }
        xpc_connection_t c = connect_daemon(q, NULL);
        xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(m, "cmd", "set-config");
        xpc_dictionary_set_int64(m, "input", in);
        xpc_dictionary_set_int64(m, "analog_mode", argc > 3 ? atoi(argv[3]) : -1);
        xpc_dictionary_set_int64(m, "h_offset", argc > 4 ? atoi(argv[4]) : 0);
        xpc_dictionary_set_int64(m, "v_offset", argc > 5 ? atoi(argv[5]) : 0);
        xpc_dictionary_set_int64(m, "tv_standard", argc > 6 ? atoi(argv[6]) : HDCAP_STD_AUTO);
        print_status(request(c, m));
        return 0;
    }
    if (!strcmp(cmd, "drift")) {
        /* Card audio clock vs host clock: least-squares fit of the ring's write position against
         * its host timestamp, sampled every second. */
        int seconds = argc > 2 ? atoi(argv[2]) : 600;
        xpc_connection_t c = connect_daemon(q, NULL);
        xpc_object_t hello = xpc_dictionary_create(NULL, NULL, 0);
        xpc_dictionary_set_string(hello, "cmd", "hello");
        xpc_dictionary_set_string(hello, "client", "hdcapd-client drift");
        xpc_dictionary_set_bool(hello, "video", false);
        xpc_dictionary_set_bool(hello, "audio", true);
        xpc_object_t r = request(c, hello);
        void *addr = NULL;
        xpc_object_t shm = xpc_dictionary_get_value(r, "audio_ring");
        const hdcapd_audio_ring *ring = shm && xpc_shmem_map(shm, &addr) >= HDCAPD_RING_BYTES ? addr : NULL;
        if (!ring || ring->magic != HDCAPD_RING_MAGIC) {
            fprintf(stderr, "no audio ring\n");
            return 1;
        }
        sleep(3); /* let the stream settle */
        double n = 0, sx = 0, sy = 0, sxx = 0, sxy = 0, x0 = 0, y0 = 0;
        uint32_t gen = atomic_load(&ring->generation);
        for (int t = 1; t <= seconds; t++) {
            sleep(1);
            uint64_t w, ts;
            do { /* consistent pair: re-read if the writer advanced in between */
                w = atomic_load(&ring->write_frames);
                ts = atomic_load(&ring->write_time_ns);
            } while (w != atomic_load(&ring->write_frames));
            if (atomic_load(&ring->generation) != gen) {
                printf("stream restarted; fit reset\n");
                gen = atomic_load(&ring->generation);
                n = sx = sy = sxx = sxy = 0;
            }
            if (n == 0) { x0 = ts / 1e9; y0 = (double)w; }
            double x = ts / 1e9 - x0, y = (double)w - y0;
            n++; sx += x; sy += y; sxx += x * x; sxy += x * y;
            if (n >= 10 && (t % 30 == 0 || t == seconds)) {
                double rate = (n * sxy - sx * sy) / (n * sxx - sx * sx);
                printf("%5ds  card rate %.3f Hz  drift %+.1f ppm vs host\n", t, rate, (rate / 48000.0 - 1) * 1e6);
                fflush(stdout);
            }
        }
        xpc_connection_cancel(c);
        return 0;
    }
    if (strcmp(cmd, "watch") != 0) {
        fprintf(stderr, "usage: hdcapd-client status | watch [SECONDS] [OUT.ppm] | drift [SECONDS] | set INPUT [MODE H V STD]\n");
        return 2;
    }

    int seconds = argc > 2 ? atoi(argv[2]) : 10;
    const char *ppm = argc > 3 ? argv[3] : NULL;
    __block uint64_t frames = 0, last_seq = 0, gaps = 0;
    __block int saved = 0, last_w = 0, last_h = 0;
    xpc_connection_t c = connect_daemon(q, ^(xpc_object_t ev) {
        if (xpc_get_type(ev) != XPC_TYPE_DICTIONARY)
            return;
        const char *type = xpc_dictionary_get_string(ev, "type");
        if (type && !strcmp(type, "status")) {
            printf("  [status] ");
            print_status(ev);
        }
        if (!type || strcmp(type, "frame") != 0)
            return;
        uint64_t seq = xpc_dictionary_get_uint64(ev, "seq");
        if (frames && seq != last_seq + 1)
            gaps += seq - last_seq - 1;
        last_seq = seq;
        frames++;
        IOSurfaceRef s = IOSurfaceLookupFromXPCObject(xpc_dictionary_get_value(ev, "surface"));
        if (!s)
            return;
        last_w = (int)IOSurfaceGetWidth(s);
        last_h = (int)IOSurfaceGetHeight(s);
        if (ppm && !saved && frames > 10) {
            IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
            size_t stride = IOSurfaceGetBytesPerRow(s);
            uint8_t *yuyv = malloc((size_t)last_w * last_h * 2), *rgb = malloc((size_t)last_w * last_h * 3);
            for (int y = 0; y < last_h; y++)
                memcpy(yuyv + (size_t)y * last_w * 2, (uint8_t *)IOSurfaceGetBaseAddress(s) + y * stride, (size_t)last_w * 2);
            IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
            hdcap_yuyv_to_rgb24(yuyv, rgb, last_w, last_h);
            FILE *f = fopen(ppm, "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", last_w, last_h);
                fwrite(rgb, 1, (size_t)last_w * last_h * 3, f);
                fclose(f);
                printf("  saved %s (%dx%d)\n", ppm, last_w, last_h);
            }
            free(yuyv);
            free(rgb);
            saved = 1;
        }
        CFRelease(s);
    });
    xpc_object_t hello = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(hello, "cmd", "hello");
    xpc_dictionary_set_string(hello, "client", "hdcapd-client");
    xpc_dictionary_set_bool(hello, "video", true);
    xpc_dictionary_set_bool(hello, "audio", true);
    xpc_object_t r = request(c, hello);
    printf("hello: ");
    print_status(r);

    const hdcapd_audio_ring *ring = NULL;
    xpc_object_t shm = xpc_dictionary_get_value(r, "audio_ring");
    void *addr = NULL;
    if (shm && xpc_shmem_map(shm, &addr) >= HDCAPD_RING_BYTES)
        ring = addr;
    if (!ring || ring->magic != HDCAPD_RING_MAGIC)
        printf("warning: no audio ring\n");

    uint64_t prev_frames = 0, prev_audio = ring ? atomic_load(&ring->write_frames) : 0;
    for (int t = 1; t <= seconds; t++) {
        sleep(1);
        __block uint64_t f;
        dispatch_sync(q, ^{ f = frames; });
        uint64_t a = ring ? atomic_load(&ring->write_frames) : 0;
        double rms = 0;
        uint64_t n = a - prev_audio;
        if (ring && n > 0 && n < HDCAPD_RING_FRAMES) {
            for (uint64_t i = prev_audio; i < a; i++) {
                int16_t l = ring->data[(i % HDCAPD_RING_FRAMES) * 2];
                rms += (double)l * l;
            }
            rms = sqrt(rms / (double)n);
        }
        printf("%3ds  %3llu frames/s  %dx%d  seq gaps %llu  audio %6llu frames/s  rms L %.1f\n", t, f - prev_frames, last_w,
               last_h, gaps, n, rms);
        prev_frames = f;
        prev_audio = a;
    }
    xpc_connection_cancel(c);
    return 0;
}
