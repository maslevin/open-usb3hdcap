/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * Analog (VGA / Component) timing modes: identification and MST3367 programming.
 *
 * Timings are from the public VESA DMT/CVT and CEA-861 standards. "hstart" and "vstart" are the
 * active-video start measured from the leading edge of sync (sync width + back porch).
 *
 * MST3367 registers (bank 0), from observed behaviour (docs/PROTOCOL.md):
 *   0x01, 0x02[7:4]  ADC PLL divider = h-total - 1 (12 bits)
 *   0x80[3:0], 0x81  window horizontal start (sync + back porch - 1)
 *   0x82[3:0], 0x83  window width in samples
 *   (Starts above 255 confirmed live: VGA 1024x768, start 295. An earlier layout with the start's
 *   high bits in 0x82[7:4] shifted such pictures by 256 samples. 0x80 is in the Windows tables,
 *   and mlaux/usb3hdcap uses it.)
 *   0x84             window vertical start (lines from sync, per field)
 *   0x85[5]          interlaced (field) mode;  0x85[3:0], 0x86  window height (per field)
 * Changing the width changes the line length in the USB stream; the demuxer follows automatically.
 *
 * ADC front end, by pixel clock (bank 0): 0x03 PLL gain, 0x05/0x06 clamp delay/width, 0x07 sync
 * output width, 0x39 clamp level (component). Anchors: 13.5 and 27 MHz match our Windows captures
 * (component 480i c14/c15, VGA c05); 74.25 and 148.5 MHz come from the Windows driver's component
 * table as reported by mlaux/usb3hdcap. VESA clocks between anchors take the nearest band, and HD
 * clocks on VGA reuse the component values: not yet verified on hardware.
 */
#include "hdcap.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CEA 1

static const hdcap_mode modes[] = {
    /* name                     htot hact hst  vtot vact vst  pclk MHz  int  family */
    { "640x480@60 (VESA)",       800,  640, 144, 525,  480, 35,  25.175, 0, 0 },
    { "640x480@60 (27 MHz, Dreamcast/480p-style)", 858, 640, 172, 525, 480, 40, 27.000, 0, 0 },
    { "640x480@72",              832,  640, 168, 520,  480, 31,  31.500, 0, 0 },
    { "640x480@75",              840,  640, 184, 500,  480, 19,  31.500, 0, 0 },
    { "640x480@85",              832,  640, 136, 509,  480, 28,  36.000, 0, 0 },
    { "800x600@56",             1024,  800, 200, 625,  600, 24,  36.000, 0, 0 },
    { "800x600@60",             1056,  800, 216, 628,  600, 27,  40.000, 0, 0 },
    { "800x600@72",             1040,  800, 184, 666,  600, 29,  50.000, 0, 0 },
    { "800x600@75",             1056,  800, 240, 625,  600, 24,  49.500, 0, 0 },
    { "800x600@85",             1048,  800, 216, 631,  600, 30,  56.250, 0, 0 },
    { "1024x768@60",            1344, 1024, 296, 806,  768, 35,  65.000, 0, 0 },
    { "1024x768@70",            1328, 1024, 280, 806,  768, 35,  75.000, 0, 0 },
    { "1024x768@75",            1312, 1024, 272, 800,  768, 31,  78.750, 0, 0 },
    { "1024x768@85",            1376, 1024, 304, 808,  768, 39,  94.500, 0, 0 },
    { "1152x864@60 (CVT)",      1520, 1152, 304, 895,  864, 28,  81.750, 0, 0 },
    /* 1600x900@60 (reduced blanking) and 1280x960@60 share every total (1800 x 1000, 108 MHz), so
     * line and frame rates cannot tell them apart; the first entry wins in auto mode. 1600x900 is
     * the more common of the two today; choose 1280x960 manually. */
    { "1600x900@60 (reduced blanking)", 1800, 1600, 176, 1000, 900, 99, 108.000, 0, 0 },
    { "1280x960@60",            1800, 1280, 424, 1000, 960, 39, 108.000, 0, 0 },
    { "1280x1024@60",           1688, 1280, 360, 1066, 1024, 41, 108.000, 0, 0 },
    { "1280x1024@75",           1688, 1280, 392, 1066, 1024, 41, 135.000, 0, 0 },
    { "1360x768@60",            1792, 1360, 368, 795,  768, 24,  85.500, 0, 0 },
    { "1440x900@60",            1904, 1440, 384, 934,  900, 31, 106.500, 0, 0 },
    { "1600x1200@60",           2160, 1600, 496, 1250, 1200, 49, 162.000, 0, 0 },
    /* CEA-861 (component and VGA-connected video sources). Interlaced: vactive/vstart per field. */
    { "720x480i (480i)",         858,  720, 119, 525,  240, 18,  13.500, 1, CEA },
    { "720x576i (576i)",         864,  720, 132, 625,  288, 22,  13.500, 1, CEA },
    { "720x480p (480p)",         858,  720, 122, 525,  480, 36,  27.000, 0, CEA },
    { "720x576p (576p)",         864,  720, 132, 625,  576, 44,  27.000, 0, CEA },
    { "1280x720p50",            1980, 1280, 260, 750,  720, 25,  74.250, 0, CEA },
    { "1280x720p60",            1650, 1280, 260, 750,  720, 25,  74.250, 0, CEA },
    { "1920x1080i50",           2640, 1920, 192, 1125, 540, 20,  74.250, 1, CEA },
    { "1920x1080i60",           2200, 1920, 192, 1125, 540, 20,  74.250, 1, CEA },
    { "1920x1080p50",           2640, 1920, 192, 1125, 1080, 41, 148.500, 0, CEA },
    { "1920x1080p60",           2200, 1920, 192, 1125, 1080, 41, 148.500, 0, CEA },
    /* 15 kHz progressive: 240p/288p from consoles, home computers and arcade boards, as RGB on VGA
     * or YPbPr on Component. BT.601 sampling (13.5 MHz, 858/864 samples per line, 720 wide) with the
     * horizontal placement of 480i/576i; the window holds exactly one field's 240/288 lines, so the
     * line-doubled output is a constant 720x480/576 (4:3, like Composite/S-Video 240p). Sources run
     * 262 or 263 (312 or 313) lines; matching allows for both. Appended at the end because OBS and
     * hdcapd store a manually chosen mode as its index in this table.
     * The windows are placed to hold the widest picture measured, not to centre narrower ones:
     * cropping a source loses picture, an off-centre one only needs the offset to look tidy.
     * Measured on a MiSTer FPGA, in samples from the leading edge of h-sync (13.5 MHz):
     *   X68000, 256x256 mode (264 lines, 15.838 kHz)  112..834, lines 17..256   (723 x 240)
     *   Genesis H40 (262 lines, 15.698 kHz)           168..811
     *   Amiga NTSC hires (263 lines, 15.623 kHz)      185..807
     * 240p: h-start 114 / v-start 17 puts the window at 113..832 and lines 17..256, so the X68000
     * fits whole (480i's 119/18 cropped 6 samples and a line, 133 cropped 20 samples); the Genesis
     * and Amiga then sit right of centre (margins about 55/22 and 72/26).
     * 288p: 576i's h-start; a PAL Amiga (lores, 313 lines) fits with margins 55/37. */
    { "720x240p (240p, 15 kHz)", 858,  720, 114, 262,  240, 17,  13.500, 0, CEA },
    { "720x288p (288p, 15 kHz)", 864,  720, 132, 312,  288, 22,  13.500, 0, CEA },
    /* Sharp X68000 768x512, 31 kHz, as the MiSTer FPGA core sends it: 568 lines at 34.08 kHz, 60 Hz
     * (the original machine runs the same 1104 x 568 raster at 31.5 kHz, 55 Hz; not verified here).
     * Measured live on a Human68k screen: at 1104 samples per line the picture is 768 samples wide
     * (1096 gives 763, 1112 gives 774), starting 263 samples after h-sync and 41 lines after v-sync,
     * 512 lines high. Needs the auto-phase to be stable: at phase 0, 1-pixel text flickered.
     * The core's line is a fraction of a sample off 1104 (1103 and 1105 are far worse), so the best
     * phase drifts slightly across the line: even at the best phase about 500 of 393,000 pixels of a
     * static screen change between frames (against about 12,800 at phase 0). */
    { "768x512@60 (X68000, MiSTer)", 1104, 768, 263, 568, 512, 41, 37.627, 0, 0 },
    /* Sharp X68000 24 kHz mode, as the MiSTer core sends it: 481 lines at 28.86 kHz, 60 Hz. Found
     * with the sharpness meter: scanning 800-1400 samples per line, the phase spread peaked sharply
     * at 944 (7.6 %, neighbours under 3 %); at 944 a moving game (Marble Madness) changed 534
     * pixels per frame, at the other peak, 1232, 6120. The picture starts 33 lines after v-sync and
     * runs 448 lines; that game's playfield is 576 wide from sample 288. The visible width is up to
     * the software, so the window is 640 wide from 256, centring 576 with room for wider screens. */
    { "640x448@60 (X68000 24 kHz, MiSTer)", 944, 640, 256, 481, 448, 33, 27.245, 0, 0 },
    /* Atari System 2 arcade (25 kHz medium resolution), measured on the MiSTer FPGA 720 Degrees
     * core: 417 lines at 25.0 kHz, 59.94 Hz. At 880 samples per line the picture was 704 wide, 80 %
     * of the line, so a 512-pixel line is 640 in all (16.0 MHz); at 640 a static text bar stayed
     * unchanged at 7 of 8 phases, at 639 and 641 it flickered at most of them. 512x384 from 96/24,
     * with equal margins either side when the window is widened. The attract mode moves too much
     * for the sharpness-meter scan (about 96 % spread at every setting). */
    { "512x384@60 (Atari System 2, MiSTer)", 640, 512, 96, 417, 384, 24, 15.997, 0, 0 },
};

const hdcap_mode *hdcap_modes(size_t *count)
{
    *count = sizeof modes / sizeof modes[0];
    return modes;
}

static double mode_hfreq(const hdcap_mode *m) { return m->pclk_mhz * 1e6 / m->htotal; }

const hdcap_mode *hdcap_find_mode(hdcap_input in, const hdcap_signal *s)
{
    if (!s->locked)
        return NULL;
    const hdcap_mode *best = NULL;
    double best_err = 1e9;
    for (size_t i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        const hdcap_mode *m = &modes[i];
        /* Interlaced sources report lines per field. */
        double vt = m->interlaced ? m->vtotal / 2.0 : m->vtotal;
        double hf_err = fabs(mode_hfreq(m) - s->hfreq_hz) / mode_hfreq(m);
        double vt_err = fabs(vt - s->vtotal);
        if (hf_err > 0.015 || vt_err > 3 || m->interlaced != s->interlaced)
            continue;
        double err = hf_err * 100 + vt_err;
        /* Prefer the family matching the connector: VESA on VGA, CEA on Component. The 27 MHz
         * 640x480 variant is only chosen explicitly (it is indistinguishable from VESA 640x480). */
        if ((in == HDCAP_INPUT_COMPONENT) != (m->family == CEA))
            err += 10;
        if (strstr(m->name, "27 MHz"))
            err += 100;
        if (err < best_err) {
            best_err = err;
            best = m;
        }
    }
    return best;
}

int hdcap_wants_line_double(hdcap_input in, const hdcap_signal *s)
{
    if (s->interlaced)
        return 0;
    if (in == HDCAP_INPUT_COMPOSITE || in == HDCAP_INPUT_SVIDEO)
        return 1;
    /* 15 kHz progressive on the ADC: 262/263 lines (240p) or 312/313 (288p); allow for sources that
     * add equalizing pulses to their sync. 31 kHz modes (480p, VGA) have 500+ lines and pass through. */
    if (in == HDCAP_INPUT_VGA || in == HDCAP_INPUT_COMPONENT)
        return s->vtotal > 0 && s->vtotal <= 320;
    return 0;
}

static int mst_write(hdcap *dev, uint8_t reg, uint8_t val)
{
    int r = hdcap_i2c_write(dev, HDCAP_I2C_MST3367, 0x00, 0x00); /* bank 0 */
    return r < 0 ? r : hdcap_i2c_write(dev, HDCAP_I2C_MST3367, reg, val);
}

hdcap_frontend hdcap_frontend_for(hdcap_input in, double pclk_mhz)
{
    hdcap_frontend f;
    if (pclk_mhz < 20)          /* 480i/576i, 13.5 MHz */
        f = (hdcap_frontend){ 0x08, 0x08, 0x08, 0x18, 0 };
    else if (pclk_mhz < 45)     /* 480p/576p, VGA 640x480..800x600 (25-40 MHz) */
        f = (hdcap_frontend){ 0x20, 0x08, 0x08, 0x18, 0 };
    else if (pclk_mhz < 105)    /* 720p, 1080i (74.25 MHz); VESA 50-95 MHz */
        f = (hdcap_frontend){ 0x58, 0x38, 0x20, 0x30, 0 };
    else                        /* 1080p (148.5 MHz); VESA 105-162 MHz */
        f = (hdcap_frontend){ 0xE8, 0x38, 0x30, 0xA0, 0 };
    if (in == HDCAP_INPUT_COMPONENT) {
        f.clamp_level = (f.clamp_delay + f.clamp_width - 0x78) & 0xFF;
    } else {
        /* VGA (RGB) clamps differently (0x2C/0x39 from the input table); at SD clocks keep the
         * sync width the VGA table uses (0x20, capture c05) */
        f.clamp_level = -1;
        if (pclk_mhz < 45)
            f.hsync_width = 0;
    }
    return f;
}

int hdcap_set_mode(hdcap *dev, hdcap_input in, const hdcap_mode *m, int hoffset, int voffset)
{
    uint8_t r02;
    int r;
    hdcap_frontend fe = hdcap_frontend_for(in, m->pclk_mhz);
    if ((r = mst_write(dev, 0x03, fe.pll_gain)) < 0 || (r = mst_write(dev, 0x05, fe.clamp_delay)) < 0 ||
        (r = mst_write(dev, 0x06, fe.clamp_width)) < 0 ||
        (fe.hsync_width && (r = mst_write(dev, 0x07, fe.hsync_width)) < 0) ||
        (fe.clamp_level >= 0 && (r = mst_write(dev, 0x39, (uint8_t)fe.clamp_level)) < 0))
        return r;
    unsigned div = (unsigned)m->htotal - 1;
    int hs = m->hstart - 1 + hoffset, vs = m->vstart + voffset;
    if (hs < 0) hs = 0;
    if (vs < 0) vs = 0;
    if ((r = hdcap_i2c_write(dev, HDCAP_I2C_MST3367, 0x00, 0x00)) < 0 ||
        (r = hdcap_i2c_read(dev, HDCAP_I2C_MST3367, 0x02, &r02)) < 0)
        return r;
    if ((r = mst_write(dev, 0x01, (uint8_t)(div >> 4))) < 0 ||
        (r = mst_write(dev, 0x02, (uint8_t)(((div & 0x0F) << 4) | (r02 & 0x0F)))) < 0 ||
        (r = mst_write(dev, 0x80, (uint8_t)((hs >> 8) & 0x0F))) < 0 ||
        (r = mst_write(dev, 0x81, (uint8_t)hs)) < 0 ||
        (r = mst_write(dev, 0x82, (uint8_t)((m->hactive >> 8) & 0x0F))) < 0 ||
        (r = mst_write(dev, 0x83, (uint8_t)m->hactive)) < 0 ||
        (r = mst_write(dev, 0x84, (uint8_t)vs)) < 0 ||
        (r = mst_write(dev, 0x85, (uint8_t)((m->interlaced ? 0x20 : 0x00) | ((m->vactive >> 8) & 0x0F)))) < 0 ||
        (r = mst_write(dev, 0x86, (uint8_t)m->vactive)) < 0)
        return r;
    return 0;
}

/* ---------- ADC sampling phase ----------
 * Bank 0 0x04 bits 7:2 = ADC sampling phase (64 steps across one sample period). The MST3367 has a
 * sharpness meter: 0xE0 = 0x80 starts it, 0x40 latches it, and 0xE9[1:0]:0xE8:0xE7:0xE6 is a 26-bit
 * total that peaks when samples land in the middle of the source's pixels. 0x1E/0x1F are cleared
 * while measuring and restored afterwards. Observed behaviour of the vendor driver's auto-phase,
 * verified live: on a MiSTer X68000 at 768x512 (1104 samples per line) the meter peaked at phase 24,
 * the same phase at which consecutive frames of a static screen stopped differing (14 changed
 * pixels, against 12,800 at phase 0). */

static int mst_read(hdcap *dev, uint8_t reg, uint8_t *val)
{
    int r = hdcap_i2c_write(dev, HDCAP_I2C_MST3367, 0x00, 0x00); /* bank 0 */
    return r < 0 ? r : hdcap_i2c_read(dev, HDCAP_I2C_MST3367, reg, val);
}

int hdcap_set_phase(hdcap *dev, int phase)
{
    return mst_write(dev, 0x04, (uint8_t)((phase & 0x3F) << 2));
}

/* Phase measured at sweep step i: the coarse steps cover all 64 phases, the fine steps the 7 around
 * the coarse winner (whose position the coarse grid only knows to within +-2). */
static int sweep_phase(const hdcap_phase_sweep *sw, int i)
{
    static const int check_offset[HDCAP_PHASE_CHECK_STEPS] = { -HDCAP_PHASE_CHECK_SPAN, HDCAP_PHASE_CHECK_SPAN, 0 };
    if (sw->check == 2)
        return sw->res.phase;   /* level reading: the phase in use */
    if (sw->check)
        return (sw->res.phase + check_offset[i] + 64) & 63;
    if (i < HDCAP_PHASE_STEPS)
        return i * 64 / HDCAP_PHASE_STEPS;
    if (i == HDCAP_PHASE_STEPS)
        return 0;   /* the first coarse phase again: did the picture change during the pass? */
    return (sw->res.coarse_phase + i - HDCAP_PHASE_STEPS - 1 - HDCAP_PHASE_FINE / 2 + 64) & 63;
}

/* Start measuring phase sw->step: set the phase and start the meter; it can be latched 50 ms later
 * (at least one frame at any supported rate). */
static int sweep_measure(hdcap *dev, hdcap_phase_sweep *sw, uint64_t now_ns)
{
    int r;
    if ((r = hdcap_set_phase(dev, sweep_phase(sw, sw->step))) < 0 || (r = mst_write(dev, 0xE0, 0x80)) < 0)
        return r;
    sw->due_ns = now_ns + 50000000ULL;
    return 0;
}

/* Restore the meter registers (also after an error) and end the sweep. */
static int sweep_end(hdcap *dev, hdcap_phase_sweep *sw, int r)
{
    int r2 = mst_write(dev, 0xE0, 0);
    if (r2 >= 0) r2 = mst_write(dev, 0x1E, sw->s1e);
    if (r2 >= 0) r2 = mst_write(dev, 0x1F, sw->s1f);
    sw->active = 0;
    return r < 0 ? r : r2;
}

static int sweep_begin(hdcap *dev, hdcap_phase_sweep *sw, uint64_t now_ns)
{
    int r;
    if ((r = mst_read(dev, 0x1E, &sw->s1e)) < 0 || (r = mst_read(dev, 0x1F, &sw->s1f)) < 0)
        return r;
    sw->active = 1;
    if ((r = mst_write(dev, 0x1E, 0)) < 0 || (r = mst_write(dev, 0x1F, 0)) < 0 || (r = mst_write(dev, 0xE0, 0)) < 0 ||
        (r = sweep_measure(dev, sw, now_ns)) < 0)
        return sweep_end(dev, sw, r);
    return 0;
}

int hdcap_phase_sweep_start(hdcap *dev, hdcap_phase_sweep *sw, uint64_t now_ns)
{
    memset(sw, 0, sizeof *sw);
    return sweep_begin(dev, sw, now_ns);
}

int hdcap_phase_sweep_abort(hdcap *dev, hdcap_phase_sweep *sw)
{
    return sw->active ? sweep_end(dev, sw, 0) : 0;
}

int hdcap_phase_check_start(hdcap *dev, hdcap_phase_sweep *sw, int phase, uint64_t now_ns)
{
    memset(sw, 0, sizeof *sw);
    sw->check = 1;
    sw->res.phase = phase & 63;
    return sweep_begin(dev, sw, now_ns);
}

int hdcap_phase_level_start(hdcap *dev, hdcap_phase_sweep *sw, int phase, uint64_t now_ns)
{
    memset(sw, 0, sizeof *sw);
    sw->check = 2;
    sw->res.phase = phase & 63;
    return sweep_begin(dev, sw, now_ns);
}

int hdcap_phase_sweep_poll(hdcap *dev, hdcap_phase_sweep *sw, uint64_t now_ns)
{
    uint8_t b[4];
    int r;
    if (!sw->active)
        return 1;
    if (now_ns < sw->due_ns)
        return 0;
    if ((r = mst_write(dev, 0xE0, 0x40)) < 0 || (r = mst_read(dev, 0xE6, &b[0])) < 0 || (r = mst_read(dev, 0xE7, &b[1])) < 0 ||
        (r = mst_read(dev, 0xE8, &b[2])) < 0 || (r = mst_read(dev, 0xE9, &b[3])) < 0)
        return sweep_end(dev, sw, r);
    uint32_t v = ((uint32_t)(b[3] & 3) << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0];
    if (sw->check == 2) {
        /* level reading: one measurement at the phase in use, which never changed */
        sw->res.fine[2] = v;
        sw->res.check_ok = 1;
        return (r = sweep_end(dev, sw, 0)) < 0 ? r : 1;
    }
    if (sw->check) {
        /* neighbours first, the current phase last, so the picture ends where it started */
        sw->res.fine[sw->step++] = v;
        if (sw->step < HDCAP_PHASE_CHECK_STEPS)
            return (r = sweep_measure(dev, sw, now_ns)) < 0 ? sweep_end(dev, sw, r) : 0;
        if ((r = sweep_end(dev, sw, 0)) < 0)
            return r;
        uint32_t side = sw->res.fine[0] > sw->res.fine[1] ? sw->res.fine[0] : sw->res.fine[1];
        sw->res.check_ok = (uint64_t)side * 1000 <= (uint64_t)sw->res.fine[2] * (1000 + HDCAP_PHASE_CHECK_MARGIN);
        return (r = hdcap_set_phase(dev, sw->res.phase)) < 0 ? r : 1;
    }
    if (sw->step < HDCAP_PHASE_STEPS)
        sw->res.value[sw->step] = v;
    else if (sw->step == HDCAP_PHASE_STEPS)
        sw->res.repeat = v;
    else
        sw->res.fine[sw->step - HDCAP_PHASE_STEPS - 1] = v;
    sw->step++;
    if (sw->step == HDCAP_PHASE_STEPS + 1) {
        /* coarse pass done: refine around its peak, unless there was none, or the picture changed
         * while it ran (a screen still being drawn makes later phases read higher, a false peak) */
        hdcap_phase_choose(&sw->res);
        uint32_t a = sw->res.value[0], b = sw->res.repeat, d = a > b ? a - b : b - a;
        if (sw->res.found && (uint64_t)d * 1000 > (uint64_t)(a > b ? a : b) * HDCAP_PHASE_MAX_DRIFT) {
            sw->res.found = 0;
            sw->res.unsteady = 1;
            sw->res.phase = 0;
        }
        sw->res.coarse_phase = sw->res.phase;
        if (!sw->res.found)
            sw->step = HDCAP_PHASE_STEPS + 1 + HDCAP_PHASE_FINE;
    }
    if (sw->step < HDCAP_PHASE_STEPS + 1 + HDCAP_PHASE_FINE)
        return (r = sweep_measure(dev, sw, now_ns)) < 0 ? sweep_end(dev, sw, r) : 0;
    if ((r = sweep_end(dev, sw, 0)) < 0)
        return r;
    if (sw->res.found) {
        int best = 0;
        for (int i = 1; i < HDCAP_PHASE_FINE; i++)
            if (sw->res.fine[i] > sw->res.fine[best])
                best = i;
        sw->res.phase = sweep_phase(sw, HDCAP_PHASE_STEPS + 1 + best);
    }
    return (r = hdcap_set_phase(dev, sw->res.phase)) < 0 ? r : 1;
}

int hdcap_auto_phase(hdcap *dev, hdcap_phase_result *res)
{
    hdcap_phase_sweep sw;
    int r;
    if ((r = hdcap_phase_sweep_start(dev, &sw, hdcap_now_ns())) < 0)
        return r;
    do {
        struct timespec ts = { 0, 5 * 1000000L };
        nanosleep(&ts, NULL);
    } while ((r = hdcap_phase_sweep_poll(dev, &sw, hdcap_now_ns())) == 0);
    if (r < 0)
        return r;
    *res = sw.res;
    return 0;
}

void hdcap_phase_choose(hdcap_phase_result *res)
{
    int best = 0;
    res->best = res->worst = res->value[0];
    for (int i = 1; i < HDCAP_PHASE_STEPS; i++) {
        if (res->value[i] > res->best) {
            res->best = res->value[i];
            best = i;
        }
        if (res->value[i] < res->worst)
            res->worst = res->value[i];
    }
    /* A flat picture (no detail, or no signal) gives no peak: keep phase 0 rather than noise. */
    res->found = res->best > 0 && (uint64_t)(res->best - res->worst) * 1000 >= (uint64_t)res->best * HDCAP_PHASE_MIN_SPREAD;
    res->phase = res->found ? best * 64 / HDCAP_PHASE_STEPS : 0;
}

double hdcap_estimate_rate(hdcap_input in, const hdcap_signal *s, const hdcap_mode *analog_mode)
{
    if (!s->locked)
        return 0;
    if (in == HDCAP_INPUT_COMPOSITE || in == HDCAP_INPUT_SVIDEO)
        return s->hfreq_hz * (1440 + 4) + 48000 * 4.5;
    /* HDMI/DVI report their active width: exact for any mode, VESA/CEA or not */
    if (!analog_mode && s->hactive > 0)
        return s->hfreq_hz * (2.0 * s->hactive + 4) + 48000 * 4.5;
    const hdcap_mode *m = analog_mode;
    if (!m) {
        /* Digital inputs: the source's own active width; take it from the matching standard mode. */
        hdcap_input as = in == HDCAP_INPUT_COMPONENT ? HDCAP_INPUT_COMPONENT : HDCAP_INPUT_VGA;
        hdcap_signal t = *s;
        m = hdcap_find_mode(as, &t);
        if (!m) {
            t.interlaced = !t.interlaced; /* digital sources report interlace differently; try both */
            m = hdcap_find_mode(as, &t);
        }
    }
    if (!m)
        return 0;
    return s->hfreq_hz * (2.0 * m->hactive + 4) + 48000 * 4.5;
}
