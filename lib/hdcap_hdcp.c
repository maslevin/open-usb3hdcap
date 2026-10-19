/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * HDCP compliance. The MST3367's built-in HDCP receiver authenticates with HDMI/DVI sources (the
 * keys live in the receiver; this driver never touches them). A capture device is an unprotected
 * output, so while a source has HDCP encryption active the driver must not deliver its picture or
 * sound: the capture session replaces frames with a notice and mutes audio until the source stops
 * encrypting. This file provides the status check and the notice frame.
 *
 * Status (bank 1; meanings per stoth68000/hdcapm): 0x01 bit 0 = HDCP authenticated/active,
 * 0x34 bit 7 = HDCP in use. Either bit set counts as protected.
 */
#include "hdcap.h"

#include <string.h>

int hdcap_hdcp_status(hdcap *dev, uint8_t raw[2])
{
    uint8_t a = 0, b = 0;
    int r;
    if ((r = hdcap_i2c_write(dev, HDCAP_I2C_MST3367, 0x00, 0x01)) < 0 ||
        (r = hdcap_i2c_read(dev, HDCAP_I2C_MST3367, 0x01, &a)) < 0 ||
        (r = hdcap_i2c_read(dev, HDCAP_I2C_MST3367, 0x34, &b)) < 0)
        return r;
    hdcap_i2c_write(dev, HDCAP_I2C_MST3367, 0x00, 0x00);   /* back to bank 0 */
    if (raw) {
        raw[0] = a;
        raw[1] = b;
    }
    return (a & 0x01) || (b & 0x80);
}

/* 5x7 glyphs for the notice text (rows top to bottom, bit 4 = leftmost column). */
static const struct { char c; uint8_t rows[7]; } font[] = {
    { 'A', { 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } }, { 'B', { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E } },
    { 'C', { 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E } }, { 'D', { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E } },
    { 'E', { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F } }, { 'H', { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 } },
    { 'I', { 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E } }, { 'K', { 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 } },
    { 'L', { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F } }, { 'N', { 0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11 } },
    { 'O', { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } }, { 'P', { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 } },
    { 'R', { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 } }, { 'S', { 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E } },
    { 'T', { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 } }, { 'U', { 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E } },
    { '-', { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 } }, { ' ', { 0 } },
};

static const uint8_t *glyph(char c)
{
    for (size_t i = 0; i < sizeof font / sizeof font[0]; i++)
        if (font[i].c == c)
            return font[i].rows;
    return font[sizeof font / sizeof font[0] - 1].rows;
}

static void draw_line(uint8_t *yuyv, int w, int h, const char *text, int cy, int scale)
{
    int n = (int)strlen(text), cw = 6 * scale, x0 = (w - n * cw + scale) / 2, y0 = cy - 7 * scale / 2;
    for (int i = 0; i < n; i++) {
        const uint8_t *g = glyph(text[i]);
        for (int gy = 0; gy < 7; gy++)
            for (int gx = 0; gx < 5; gx++) {
                if (!(g[gy] & (0x10 >> gx)))
                    continue;
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++) {
                        int x = x0 + i * cw + gx * scale + dx, y = y0 + gy * scale + dy;
                        if (x >= 0 && x < w && y >= 0 && y < h)
                            yuyv[((size_t)y * w + x) * 2] = 235;   /* luma byte of the pixel */
                    }
            }
    }
}

void hdcap_render_hdcp_notice(uint8_t *yuyv, int width, int height)
{
    for (size_t i = 0; i < (size_t)width * height * 2; i += 2) {
        yuyv[i] = 16;        /* black */
        yuyv[i + 1] = 128;   /* no colour */
    }
    int scale = width / 160 > 1 ? width / 160 : 1;
    if (scale > 12)
        scale = 12;
    draw_line(yuyv, width, height, "HDCP-PROTECTED SOURCE", height / 2 - 6 * scale, scale);
    draw_line(yuyv, width, height, "CAPTURE BLOCKED", height / 2 + 6 * scale, scale);
}
