/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * mst_decode_test: MST3367 timing-register decoding and mode matching, checked against register
 * values the Windows driver read in USB captures (c05 VGA, c11 HDMI, c14/c15 component).
 */
#include "hdcap.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

void hdcap__mst_decode(const uint8_t raw[8], hdcap_signal *s);

struct tc {
    const char *what;
    hdcap_input in;
    uint8_t raw[8];   /* 0x55 0x57 0x58 0x59 0x5A 0x5B 0x5C 0x5F */
    double khz, hz;
    int vtotal, interlaced;
    const char *mode; /* expected hdcap_find_mode() name, or NULL */
};

static const struct tc cases[] = {
    { "c05 VGA 640x480@60",       HDCAP_INPUT_VGA,       { 0x03, 0x13, 0xE2, 0x08, 0x27, 0x02, 0x0D, 0x00 }, 31.47, 59.94, 525, 0, "640x480@60 (VESA)" },
    { "c11 HDMI 720p60",          HDCAP_INPUT_HDMI,      { 0x7F, 0x0D, 0xE7, 0x08, 0x25, 0x02, 0xEE, 0x00 }, 45.00, 60.00, 750, 0, NULL },
    { "c14 component 480p",       HDCAP_INPUT_COMPONENT, { 0x03, 0x13, 0xE1, 0x08, 0x27, 0x02, 0x0D, 0x00 }, 31.47, 59.94, 525, 0, "720x480p (480p)" },
    { "c14 component 480i (A)",   HDCAP_INPUT_COMPONENT, { 0x03, 0x27, 0xC4, 0x08, 0x27, 0x01, 0x0F, 0x0A }, 15.73, 59.94, 262, 1, "720x480i (480i)" },
    { "c15 component 480i (B)",   HDCAP_INPUT_COMPONENT, { 0x03, 0x27, 0xC3, 0x08, 0x27, 0x01, 0x10, 0x0F }, 15.73, 59.94, 262, 1, "720x480i (480i)" },
    { "no signal (saturated)",    HDCAP_INPUT_VGA,       { 0x03, 0x3F, 0xFF, 0x3F, 0xFF, 0x07, 0xFF, 0x00 }, 0, 0, 0, 0, NULL },
};

int main(void)
{
    int fails = 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        const struct tc *c = &cases[i];
        hdcap_signal s;
        memset(&s, 0, sizeof s);
        hdcap__mst_decode(c->raw, &s);
        const hdcap_mode *m = hdcap_find_mode(c->in, &s);
        int ok;
        if (c->khz == 0)
            ok = !s.locked;
        else
            ok = s.locked && fabs(s.hfreq_hz / 1000 - c->khz) < 0.05 && fabs(s.vfreq_hz - c->hz) < 0.1 &&
                 s.vtotal == c->vtotal && s.interlaced == c->interlaced &&
                 (!c->mode || (m && !strcmp(m->name, c->mode)));
        printf("%-26s %s  locked %d  %7.3f kHz  %6.2f Hz  %4d lines%s  mode %s\n", c->what, ok ? "OK  " : "FAIL",
               s.locked, s.hfreq_hz / 1000, s.vfreq_hz, s.vtotal, s.interlaced ? " interlaced" : "", m ? m->name : "-");
        fails += !ok;
    }
    printf("%s\n", fails ? "FAILURES" : "ALL OK");
    return fails != 0;
}
