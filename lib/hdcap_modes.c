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
