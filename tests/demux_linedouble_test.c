/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * demux_linedouble_test: synthetic 240p stream (field bit constant, as a progressive source may
 * send) through the demuxer, with and without line doubling.
 */
#include "hdcap.h"
#include <stdio.h>
#include <string.h>

#define W 8          /* pixels per line */
#define LINES 240

static int frames, last_h, last_interlaced, doubled_ok;

static void on_frame(const hdcap_frame *f, void *user)
{
    (void)user;
    frames++;
    last_h = f->height;
    last_interlaced = f->interlaced;
    doubled_ok = 1;
    for (int y = 0; y + 1 < f->height; y += 2)
        if (memcmp(f->yuyv + (size_t)y * W * 2, f->yuyv + (size_t)(y + 1) * W * 2, W * 2) ||
            f->yuyv[(size_t)y * W * 2] != (uint8_t)(16 + (y / 2) % 200))
            doubled_ok = 0;
}

static void feed_line(hdcap_demux *dx, uint8_t xy, uint8_t luma)
{
    uint8_t rec[4 + W * 2] = { 0xFF, 0x00, 0x00, xy };
    for (int i = 0; i < W; i++) {
        rec[4 + i * 2] = luma;
        rec[5 + i * 2] = 0x80;
    }
    hdcap_demux_feed(dx, rec, sizeof rec);
}

static void feed_fields(hdcap_demux *dx, int n)
{
    for (int f = 0; f < n; f++) {
        for (int i = 0; i < 20; i++)
            feed_line(dx, 0xAB, 16);                 /* vertical blanking, field 0 */
        for (int y = 0; y < LINES; y++)
            feed_line(dx, 0x80, (uint8_t)(16 + y % 200)); /* active, field 0 */
    }
    for (int i = 0; i < 20; i++)
        feed_line(dx, 0xAB, 16);
}

int main(void)
{
    int fails = 0;
    hdcap_demux *dx = hdcap_demux_new(on_frame, NULL, NULL);
    feed_fields(dx, 5);
    printf("default:      %d frames, %d lines, interlaced %d\n", frames, last_h, last_interlaced);
    fails += !(frames >= 3 && last_h == LINES);
    hdcap_demux_free(dx);

    frames = 0;
    dx = hdcap_demux_new(on_frame, NULL, NULL);
    hdcap_demux_set_line_double(dx, 1);
    feed_fields(dx, 5);
    printf("line-doubled: %d frames, %d lines, interlaced %d, lines duplicated %s\n", frames, last_h, last_interlaced,
           doubled_ok ? "yes" : "NO");
    fails += !(frames >= 3 && last_h == 2 * LINES && !last_interlaced && doubled_ok);
    hdcap_demux_free(dx);
    printf("%s\n", fails ? "FAILURES" : "ALL OK");
    return fails != 0;
}
