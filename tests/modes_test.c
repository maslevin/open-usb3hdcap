/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * modes_test: which analog mode hdcap_find_mode() picks for the 15 kHz and X68000 signals added
 * after v0.3.1 (and that the neighbouring 31 kHz and interlaced matches are unchanged), the table
 * layout rules they depend on, and the auto-phase decision (hdcap_phase_choose).
 *
 *   cc -Iinclude $(pkg-config --cflags libusb-1.0) -o modes_test tests/modes_test.c \
 *      libhdcap.a $(pkg-config --libs libusb-1.0) && ./modes_test
 */
#include "hdcap.h"
#include <stdio.h>
#include <string.h>

static int fails;

static void expect(hdcap_input in, int vtotal, double khz, int interlaced, const char *want, const char *what)
{
    hdcap_signal s;
    memset(&s, 0, sizeof s);
    s.locked = 1;
    s.vtotal = vtotal;
    s.hfreq_hz = khz * 1000;
    s.interlaced = interlaced;
    const hdcap_mode *m = hdcap_find_mode(in, &s);
    const char *got = m ? m->name : "none";
    int ok = want ? (m && strstr(got, want) != NULL) : !m;
    printf("%-52s -> %-28s%s\n", what, got, ok ? "" : "  <-- WRONG");
    fails += !ok;
}

int main(void)
{
    /* 240p: 262 or 263 lines depending on the source */
    expect(HDCAP_INPUT_VGA, 263, 15.623, 0, "240p, 15 kHz", "VGA RGB, MiSTer 263 lines, 15.623 kHz (seen live)");
    expect(HDCAP_INPUT_VGA, 262, 15.734, 0, "240p, 15 kHz", "VGA RGB, 262 lines, 15.734 kHz");
    expect(HDCAP_INPUT_VGA, 262, 15.700, 0, "240p, 15 kHz", "VGA RGB, 262 lines, 15.700 kHz");
    expect(HDCAP_INPUT_COMPONENT, 263, 15.734, 0, "240p, 15 kHz", "Component, 263 lines, 15.734 kHz");
    /* 288p */
    expect(HDCAP_INPUT_VGA, 312, 15.625, 0, "288p, 15 kHz", "VGA RGB, 312 lines, 15.625 kHz");
    expect(HDCAP_INPUT_VGA, 313, 15.625, 0, "288p, 15 kHz", "VGA RGB, 313 lines, 15.625 kHz");
    expect(HDCAP_INPUT_COMPONENT, 312, 15.625, 0, "288p, 15 kHz", "Component, 312 lines, 15.625 kHz");
    /* unchanged neighbours */
    expect(HDCAP_INPUT_VGA, 525, 31.469, 0, "640x480@60 (VESA)", "VGA 640x480@60 (unchanged)");
    expect(HDCAP_INPUT_COMPONENT, 525, 31.469, 0, "480p", "Component 480p (unchanged)");
    expect(HDCAP_INPUT_COMPONENT, 262, 15.734, 1, "480i", "Component 480i, 262 lines per field (unchanged)");
    expect(HDCAP_INPUT_VGA, 400, 15.734, 0, NULL, "VGA 400 lines at 15.7 kHz: no match (unchanged)");
    /* X68000 on MiSTer: the 15 kHz 256x256 mode and the 31 kHz 768x512 mode (seen live) */
    expect(HDCAP_INPUT_VGA, 264, 15.838, 0, "240p, 15 kHz", "X68000 15 kHz (264 lines, 15.838 kHz, seen live)");
    expect(HDCAP_INPUT_VGA, 568, 34.082, 0, "X68000, MiSTer", "X68000 768x512 (568 lines, 34.082 kHz, seen live)");
    expect(HDCAP_INPUT_VGA, 481, 28.861, 0, "X68000 24 kHz", "X68000 24 kHz (481 lines, 28.861 kHz, seen live)");
    expect(HDCAP_INPUT_VGA, 417, 24.996, 0, "Atari System 2", "Atari System 2, 720 Degrees (417 lines, 24.996 kHz, seen live)");

    /* Auto-phase decision, from meter readings (the X68000 sweep seen live, then a flat picture) */
    hdcap_phase_result ph = { 0 };
    static const uint32_t x68k[HDCAP_PHASE_STEPS] = { 102739, 104829, 105139, 106607, 109363, 110440, 111409, 110899,
                                                      110126, 109397, 108099, 105603, 103465, 101752, 103046, 103187 };
    memcpy(ph.value, x68k, sizeof x68k);
    hdcap_phase_choose(&ph);
    printf("auto-phase, X68000 sweep: phase %d found %d%s\n", ph.phase, ph.found, ph.phase == 24 && ph.found ? "" : "  <-- WRONG");
    fails += !(ph.phase == 24 && ph.found);
    for (int i = 0; i < HDCAP_PHASE_STEPS; i++)
        ph.value[i] = 50000 + (uint32_t)(i * 7 % 5) * 10; /* within 0.1 %: no real peak */
    hdcap_phase_choose(&ph);
    printf("auto-phase, flat picture: phase %d found %d%s\n", ph.phase, ph.found, ph.phase == 0 && !ph.found ? "" : "  <-- WRONG");
    fails += !(ph.phase == 0 && !ph.found);
    memset(ph.value, 0, sizeof ph.value);
    hdcap_phase_choose(&ph);
    printf("auto-phase, no signal (all zero): phase %d found %d%s\n", ph.phase, ph.found, ph.phase == 0 && !ph.found ? "" : "  <-- WRONG");
    fails += !(ph.phase == 0 && !ph.found);

    /* Manual mode choices are stored as indices (OBS settings, hdcapd.conf), so new modes must stay
     * at the end of the table; and the 15 kHz windows must hold exactly one 240/288-line field. */
    size_t n;
    const hdcap_mode *all = hdcap_modes(&n);
    int ok = n >= 37 && !strcmp(all[31].name, "1920x1080p60") && strstr(all[32].name, "240p, 15 kHz") &&
             strstr(all[33].name, "288p, 15 kHz") && strstr(all[34].name, "X68000, MiSTer") &&
             strstr(all[35].name, "X68000 24 kHz") && strstr(all[36].name, "Atari System 2") && all[32].vactive == 240 &&
             all[33].vactive == 288 && !all[32].interlaced && !all[33].interlaced && all[32].hactive == 720 &&
             all[33].hactive == 720;
    printf("v0.3.1's 32 modes keep their indices; 240p, 288p, two X68000 modes, Atari System 2 follow at 32-36%s\n", ok ? "" : "  <-- WRONG");
    fails += !ok;

    printf("%s\n", fails ? "FAILURES" : "ALL OK");
    return fails != 0;
}
