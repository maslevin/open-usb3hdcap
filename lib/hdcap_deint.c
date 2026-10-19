/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * Deinterlacer for woven frames from the demuxer (packed YUYV, field 0 on even lines and captured
 * first, field 1 on odd lines one field period later). Each woven frame becomes two progressive
 * frames, one per field, so motion keeps the full field rate (59.94 or 50 per second).
 *
 * HDCAP_DEINT_BOB: missing lines are the average of the field's lines above and below.
 * HDCAP_DEINT_ADAPTIVE: motion-adaptive, using only past fields, so it adds no latency. For a missing
 *   line it compares the field's neighbouring lines and the other field's line with their values two
 *   fields earlier. Where nothing moved it keeps the other field's real line (full vertical
 *   detail); where things moved it interpolates as in bob; in between it blends. Static
 *   detail stays sharp, without comb lines on motion.
 */
#include "hdcap.h"

#include <stdlib.h>
#include <string.h>

#define MOTION_LO 6    /* below: weave (still) */
#define MOTION_HI 24   /* above: interpolate (moving) */

struct hdcap_deinterlacer {
    int width, height;         /* geometry of the history */
    uint8_t *prev, *prev2;     /* the two previous woven frames, packed (stride = width * 2) */
    int have;                  /* how many of them are valid */
};

hdcap_deinterlacer *hdcap_deint_new(void)
{
    return calloc(1, sizeof(hdcap_deinterlacer));
}

void hdcap_deint_free(hdcap_deinterlacer *d)
{
    if (!d)
        return;
    free(d->prev);
    free(d->prev2);
    free(d);
}

void hdcap_deint_reset(hdcap_deinterlacer *d)
{
    d->have = 0;
}

static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* Build one progressive frame from field `f` (0 or 1) of the woven frame `cur`.
 *   other_now  frame holding the other field one field period earlier (for f = 1 that is `cur`
 *              itself, for f = 0 the previous woven frame), or NULL
 *   same_prev  frame holding field `f` two field periods earlier (the previous woven frame), or NULL
 *   other_prev frame holding the other field three field periods earlier, or NULL */
static void field_frame(hdcap_deint_mode mode, int f, const uint8_t *cur, int cstride, const uint8_t *other_now,
                        const uint8_t *same_prev, const uint8_t *other_prev, int w, int h, uint8_t *out, int ostride)
{
    int lb = w * 2, pstride = lb;
    for (int y = 0; y < h; y++) {
        uint8_t *o = out + (size_t)y * ostride;
        if ((y & 1) == f) {   /* this field's own line */
            memcpy(o, cur + (size_t)y * cstride, lb);
            continue;
        }
        int ya = y > 0 ? y - 1 : y + 1, yb = y + 1 < h ? y + 1 : y - 1;   /* same-field neighbours */
        const uint8_t *a = cur + (size_t)ya * cstride, *b = cur + (size_t)yb * cstride;
        if (mode == HDCAP_DEINT_BOB || !other_now) {
            for (int i = 0; i < lb; i++)
                o[i] = (uint8_t)((a[i] + b[i] + 1) >> 1);
            continue;
        }
        int ostr = other_now == cur ? cstride : pstride;
        const uint8_t *p = other_now + (size_t)y * ostr;
        const uint8_t *a2 = same_prev ? same_prev + (size_t)ya * pstride : NULL;
        const uint8_t *b2 = same_prev ? same_prev + (size_t)yb * pstride : NULL;
        const uint8_t *p2 = other_prev ? other_prev + (size_t)y * pstride : NULL;
        for (int x = 0; x < lb; x += 2) {
            /* motion from luma (bytes 0 and 2 of each YUYV pair) and applied to the pixel pair */
            int m = 255;
            if (a2 && p2) {
                m = iabs(a[x] - a2[x]);
                m = imax(m, iabs(b[x] - b2[x]));
                m = imax(m, iabs(p[x] - p2[x]));
            }
            for (int i = x; i < x + 2; i++) {
                int s = (a[i] + b[i] + 1) >> 1, wv = p[i], v;
                if (m <= MOTION_LO)
                    v = wv;
                else if (m >= MOTION_HI)
                    v = s;
                else
                    v = (wv * (MOTION_HI - m) + s * (m - MOTION_LO)) / (MOTION_HI - MOTION_LO);
                o[i] = (uint8_t)v;
            }
        }
    }
}

int hdcap_deint_process(hdcap_deinterlacer *d, hdcap_deint_mode mode, const uint8_t *woven, int stride, int width,
                        int height, uint8_t *out0, uint8_t *out1, int out_stride)
{
    size_t fsz = (size_t)width * 2 * height;
    if (width != d->width || height != d->height || !d->prev) {
        free(d->prev);
        free(d->prev2);
        d->prev = malloc(fsz);
        d->prev2 = malloc(fsz);
        if (!d->prev || !d->prev2)
            return -1;
        d->width = width;
        d->height = height;
        d->have = 0;
    }
    const uint8_t *p1 = d->have >= 1 ? d->prev : NULL, *p2 = d->have >= 2 ? d->prev2 : NULL;
    /* field 0 (earlier): other field one period ago = field 1 of the previous frame; same field two
     * periods ago = previous frame; other field three periods ago = field 1 of the frame before */
    field_frame(mode, 0, woven, stride, p1, p1, p2, width, height, out0, out_stride);
    /* field 1 (later): other field one period ago = field 0 of this frame */
    field_frame(mode, 1, woven, stride, woven, p1, p1, width, height, out1, out_stride);

    uint8_t *t = d->prev2;
    d->prev2 = d->prev;
    d->prev = t;
    for (int y = 0; y < height; y++)
        memcpy(d->prev + (size_t)y * width * 2, woven + (size_t)y * stride, (size_t)width * 2);
    if (d->have < 2)
        d->have++;
    return 2;
}
