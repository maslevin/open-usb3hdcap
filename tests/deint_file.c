/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * deint_file: run woven frames from a raw stream file through the deinterlacer and report
 *   - comb: mean positive (Y[y]-Y[y-1])*(Y[y]-Y[y+1]) over the picture (lower = less combing)
 *   - temporal order: mean |frame k - frame k-1| for odd vs even k (similar = fields in time order)
 *   - spatial order: comb of the weave as delivered vs with the fields swapped (lower = correct)
 * and save PPMs of one moving frame (woven, bob, adaptive).
 *   deint_file STREAM [SKIP_BYTES] [FRAMES] [OUTDIR]
 */
#include "hdcap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int W, H, n, want = 120;
static uint8_t *bob0, *bob1, *ada0, *ada1, *prevA, *swapped;
static hdcap_deinterlacer *dB, *dA;
static double comb_w, comb_b, comb_a, comb_sw, dodd, deven;
static int nd;
static const char *outdir;
static double best_motion; static int saved;

static double comb(const uint8_t *f, int w, int h)
{
    double s = 0; long c = 0;
    for (int y = 20; y < h - 20; y++)
        for (int x = 0; x < w * 2; x += 2) {
            int a = f[(y - 1) * w * 2 + x], m = f[y * w * 2 + x], b = f[(y + 1) * w * 2 + x];
            int v = (m - a) * (m - b);
            if (v > 0) s += v;
            c++;
        }
    return s / c;
}

static double diff(const uint8_t *p, const uint8_t *q, int w, int h)
{
    double s = 0;
    for (int i = 0; i < w * h * 2; i += 2) s += abs(p[i] - q[i]);
    return s / (w * h);
}

static void save(const uint8_t *yuyv, const char *name)
{
    if (!outdir) return;
    uint8_t *rgb = malloc((size_t)W * H * 3);
    hdcap_yuyv_to_rgb24(yuyv, rgb, W, H);
    char path[1024]; snprintf(path, sizeof path, "%s/%s.ppm", outdir, name);
    FILE *f = fopen(path, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(rgb, 1, (size_t)W * H * 3, f); fclose(f);
    free(rgb);
}

static void on_frame(const hdcap_frame *fr, void *u)
{
    (void)u;
    if (!fr->interlaced || n >= want) return;
    if (!bob0) {
        W = fr->width; H = fr->height; size_t sz = (size_t)W * H * 2;
        bob0 = malloc(sz); bob1 = malloc(sz); ada0 = malloc(sz); ada1 = malloc(sz); prevA = malloc(sz); swapped = malloc(sz);
    }
    if (fr->width != W || fr->height != H) return;
    hdcap_deint_process(dB, HDCAP_DEINT_BOB, fr->yuyv, W * 2, W, H, bob0, bob1, W * 2);
    hdcap_deint_process(dA, HDCAP_DEINT_ADAPTIVE, fr->yuyv, W * 2, W, H, ada0, ada1, W * 2);
    for (int y = 0; y + 1 < H; y += 2) {   /* swap the fields' vertical positions */
        memcpy(swapped + (size_t)y * W * 2, fr->yuyv + (size_t)(y + 1) * W * 2, W * 2);
        memcpy(swapped + (size_t)(y + 1) * W * 2, fr->yuyv + (size_t)y * W * 2, W * 2);
    }
    if (n >= 3) {   /* adaptive history warmed up */
        comb_w += comb(fr->yuyv, W, H); comb_b += comb(bob1, W, H); comb_a += comb(ada1, W, H);
        comb_sw += comb(swapped, W, H);
        /* temporal order on bob output (no field mixing): prevA = field 1 of previous frame */
        deven += diff(prevA, bob0, W, H);   /* prev F1 -> this F0 */
        dodd += diff(bob0, bob1, W, H);     /* this F0 -> this F1 */
        nd++;
        /* sample frame: moderate motion (2-6 levels mean change), not a scene cut */
        double mot = diff(prevA, bob1, W, H);
        if (outdir && mot > best_motion && mot > 2 && mot < 6 && n > 10) {
            best_motion = mot;
            save(fr->yuyv, "woven"); save(bob1, "bob"); save(ada1, "adaptive"); saved = 1;
        }
    }
    memcpy(prevA, bob1, (size_t)W * H * 2);
    n++;
}

int main(int argc, char **argv)
{
    FILE *in = fopen(argv[1], "rb");
    if (!in) return 1;
    fseek(in, argc > 2 ? atol(argv[2]) : 0, SEEK_SET);
    if (argc > 3) want = atoi(argv[3]);
    outdir = argc > 4 ? argv[4] : NULL;
    dB = hdcap_deint_new(); dA = hdcap_deint_new();
    hdcap_demux *dx = hdcap_demux_new(on_frame, NULL, NULL);
    static uint8_t buf[1 << 20]; size_t r;
    while (n < want && (r = fread(buf, 1, sizeof buf, in)) > 0) hdcap_demux_feed(dx, buf, r);
    printf("%d woven frames %dx%d\n", n, W, H);
    printf("comb: woven %.1f  bob %.1f  adaptive %.1f   (fields swapped: %.1f)\n", comb_w / nd, comb_b / nd, comb_a / nd, comb_sw / nd);
    printf("temporal steps: prevF1->F0 %.2f  F0->F1 %.2f  (ratio %.2f; ~1 = fields in time order)\n", deven / nd, dodd / nd,
           (deven / nd) / (dodd / nd));
    printf("spatial order: %s\n", comb_w < comb_sw ? "field 0 on even lines is correct" : "SWAPPED looks better");
    return 0;
}
