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

static void feed_blank(hdcap_demux *dx)
{
    for (int i = 0; i < 20; i++)
        feed_line(dx, 0xAB, 16);
}

/* One progressive field: vertical blanking, then `lines` active lines. It is completed (and a
 * frame emitted) by the blanking that starts the next field or by feed_blank(). */
static void feed_var_field(hdcap_demux *dx, int lines)
{
    feed_blank(dx);
    for (int y = 0; y < lines; y++)
        feed_line(dx, 0x80, (uint8_t)(16 + y % 200));
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

    /* Output height while the source changes modes (15 kHz RGB on VGA, default 480-line window):
     * steady 248-line fields, then fields that miss vertical sync and run to the window's end
     * (298, 480), then back to 248. The height must not stick at a glitch, and must follow a real
     * change (to 288p and back down to 240p) once it holds for half a second. */
    static const struct { int lines, count, want_h; const char *what; } steps[] = {
        { 248, 10, 496, "steady 248-line fields" },
        { 298, 1,  496, "one 298-line glitch field" },
        { 480, 3,  496, "three 480-line glitch fields" },
        { 248, 10, 496, "back to 248" },
        { 288, 40, 576, "real change to 288p (held)" },
        { 240, 40, 480, "real change down to 240p" },
        { 239, 1,  480, "SNES-style 239/240 alternation" },
        { 240, 1,  480, "SNES-style 239/240 alternation" },
    };
    dx = hdcap_demux_new(on_frame, NULL, NULL);
    hdcap_demux_set_line_double(dx, 1);
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        for (int k = 0; k < steps[i].count; k++)
            feed_var_field(dx, steps[i].lines);
        feed_blank(dx); /* completes the last field */
        printf("mode change: %-32s height %d%s\n", steps[i].what, last_h, last_h == steps[i].want_h ? "" : "  <-- WRONG");
        fails += last_h != steps[i].want_h;
    }
    hdcap_demux_free(dx);

    /* Which signals the session line-doubles (lines per field/frame as the receivers report them) */
    static const struct { hdcap_input in; int vtotal, interlaced, want; const char *what; } cases[] = {
        { HDCAP_INPUT_COMPOSITE, 262, 0, 1, "Composite 240p" },
        { HDCAP_INPUT_COMPOSITE, 262, 1, 0, "Composite 480i" },
        { HDCAP_INPUT_VGA,       262, 0, 1, "VGA 15 kHz RGB 240p" },
        { HDCAP_INPUT_VGA,       313, 0, 1, "VGA 15 kHz RGB 288p" },
        { HDCAP_INPUT_VGA,       525, 0, 0, "VGA 640x480@60" },
        { HDCAP_INPUT_VGA,       0,   0, 0, "VGA no signal" },
        { HDCAP_INPUT_COMPONENT, 263, 0, 1, "Component 240p" },
        { HDCAP_INPUT_COMPONENT, 272, 1, 0, "Component 480i" },
        { HDCAP_INPUT_COMPONENT, 525, 0, 0, "Component 480p" },
        { HDCAP_INPUT_HDMI,      262, 0, 0, "HDMI (never)" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        hdcap_signal sig = { 0 };
        sig.locked = 1;
        sig.vtotal = cases[i].vtotal;
        sig.interlaced = cases[i].interlaced;
        int got = hdcap_wants_line_double(cases[i].in, &sig);
        printf("line double %-22s %d%s\n", cases[i].what, got, got == cases[i].want ? "" : "  <-- WRONG");
        fails += got != cases[i].want;
    }
    printf("%s\n", fails ? "FAILURES" : "ALL OK");
    return fails != 0;
}
