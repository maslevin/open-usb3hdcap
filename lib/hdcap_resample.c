/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * Drift-correcting audio resampler: card clock -> host clock.
 *
 * The card's 48 kHz audio comes from the source (HDMI) or its own crystal (line-in) and runs a few
 * hundred ppm off the host clock. Consumers that timestamp with the host clock and expect
 * continuous timestamps (OBS: it resets audio timing whenever timestamps and sample counts
 * disagree by more than 70 ms) need the card audio converted to exactly the host's 48 kHz.
 *
 * Output frames are stamped on a continuous timeline, t0 + n/48000 (host clock). The input step
 * per output frame is the measured card/host rate ratio, plus a proportional correction that keeps
 * the output timeline aligned with the capture time of the samples being emitted (estimated from
 * arrival times, low-pass filtered to remove USB burst jitter). Interpolation is 4-point cubic
 * Hermite. A large disagreement (input gap, stream restart) resets the timeline.
 */
#include "hdcap.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CH            2
#define RATE          48000.0
#define FIFO_FRAMES   8192
#define MIN_RATE_SPAN 10e9      /* ns of input before trusting the measured rate */
#define GAIN_PPM_PER_MS 10.0    /* phase correction: 10 ppm per ms of error */
#define MAX_CORR_PPM  1000.0
#define RESET_ERR_NS  200e6     /* timeline reset when off by more than 200 ms */
#define FILTER_S      1.0       /* error low-pass time constant */

struct hdcap_resampler {
    int started;
    double t0_ns;               /* host time of output frame 0 */
    uint64_t out_total;         /* output frames emitted */
    uint64_t in_total;          /* input frames received */
    double first_in_ns;         /* arrival-based capture time of input frame 0 */
    double card_rate;           /* measured input rate, Hz */
    double step;                /* input frames per output frame */
    double err_ns;              /* filtered timing error (output timeline - capture time) */
    uint64_t resets;
    /* input FIFO: fifo[0] is absolute input frame fifo_base; pos is the fractional absolute
     * position of the next output frame */
    float fifo[FIFO_FRAMES * CH];
    size_t fifo_len;
    uint64_t fifo_base;
    double pos;
};

hdcap_resampler *hdcap_resampler_new(void)
{
    hdcap_resampler *r = calloc(1, sizeof *r);
    return r;
}

void hdcap_resampler_free(hdcap_resampler *r) { free(r); }

void hdcap_resampler_reset(hdcap_resampler *r)
{
    uint64_t resets = r->resets;
    memset(r, 0, sizeof *r);
    r->resets = resets;
}

void hdcap_resampler_get_stats(const hdcap_resampler *r, hdcap_resampler_stats *st)
{
    st->card_rate_hz = r->card_rate;
    st->ratio_ppm = (r->step - 1.0) * 1e6;
    st->error_ms = r->err_ns / 1e6;
    st->resets = r->resets;
}

static inline float hermite(float xm1, float x0, float x1, float x2, float t)
{
    float c1 = 0.5f * (x1 - xm1);
    float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

size_t hdcap_resampler_process(hdcap_resampler *r, const int16_t *in, size_t frames, uint64_t now_ns,
                               int16_t *out, size_t out_cap, uint64_t *out_ts_ns)
{
    if (!frames)
        return 0;
    if (!r->started) {
        r->started = 1;
        r->first_in_ns = (double)now_ns - frames * 1e9 / RATE;
        r->t0_ns = r->first_in_ns;
        r->card_rate = RATE;
        r->step = 1.0;
        r->pos = 1.0;           /* frame 0 is the first interpolation tap */
    }

    /* Timing error for the next output frame: its timeline stamp vs the capture time of the input
     * position it will be read from (the newest input frame was captured about now). */
    uint64_t in_end = r->in_total + frames;
    double cap_ns = (double)now_ns - (double)(in_end - r->pos) * 1e9 / r->card_rate;
    double out_ns = r->t0_ns + (double)r->out_total * 1e9 / RATE;
    double e = out_ns - cap_ns;
    if (fabs(e - r->err_ns) > RESET_ERR_NS && r->in_total) {
        /* input gap or restart: start a new timeline, keep nothing */
        r->resets++;
        uint64_t resets = r->resets;
        hdcap_resampler_reset(r);
        r->resets = resets;
        return hdcap_resampler_process(r, in, frames, now_ns, out, out_cap, out_ts_ns);
    }
    double alpha = frames / RATE / FILTER_S;
    if (alpha > 1)
        alpha = 1;
    r->err_ns += (e - r->err_ns) * alpha;

    /* append input to the FIFO (drop the oldest if a consumer stalls; should not happen) */
    if (r->fifo_len + frames > FIFO_FRAMES) {
        size_t drop = r->fifo_len + frames - FIFO_FRAMES;
        if (drop > r->fifo_len)
            drop = r->fifo_len;
        memmove(r->fifo, r->fifo + drop * CH, (r->fifo_len - drop) * CH * sizeof(float));
        r->fifo_len -= drop;
        r->fifo_base += drop;
    }
    size_t take = frames < FIFO_FRAMES - r->fifo_len ? frames : FIFO_FRAMES - r->fifo_len;
    for (size_t i = 0; i < take * CH; i++)
        r->fifo[r->fifo_len * CH + i] = in[i];
    r->fifo_len += take;
    r->in_total = in_end;

    /* rate: measured over the whole run once long enough; then the phase correction */
    double span = (double)now_ns - r->first_in_ns;
    if (span > MIN_RATE_SPAN)
        r->card_rate = (double)r->in_total / (span * 1e-9);
    double corr = r->err_ns / 1e6 * GAIN_PPM_PER_MS;   /* ahead -> consume input faster */
    if (corr > MAX_CORR_PPM) corr = MAX_CORR_PPM;
    if (corr < -MAX_CORR_PPM) corr = -MAX_CORR_PPM;
    r->step = r->card_rate / RATE * (1 + corr * 1e-6);

    *out_ts_ns = (uint64_t)(r->t0_ns + (double)r->out_total * 1e9 / RATE);
    size_t n = 0;
    while (n < out_cap) {
        double rel = r->pos - (double)r->fifo_base;
        size_t i = (size_t)rel;
        if (rel < 1 || i + 2 >= r->fifo_len)
            break;
        float t = (float)(rel - i);
        const float *p = r->fifo + (i - 1) * CH;
        for (int c = 0; c < CH; c++) {
            float v = hermite(p[c], p[CH + c], p[2 * CH + c], p[3 * CH + c], t);
            out[n * CH + c] = (int16_t)lrintf(v > 32767.f ? 32767.f : v < -32768.f ? -32768.f : v);
        }
        n++;
        r->pos += r->step;
    }
    r->out_total += n;

    /* discard consumed input, keeping one frame of history for the next interpolation */
    double rel = r->pos - (double)r->fifo_base;
    size_t keep_from = rel >= 2 ? (size_t)rel - 1 : 0;
    if (keep_from > r->fifo_len)
        keep_from = r->fifo_len;
    if (keep_from) {
        memmove(r->fifo, r->fifo + keep_from * CH, (r->fifo_len - keep_from) * CH * sizeof(float));
        r->fifo_len -= keep_from;
        r->fifo_base += keep_from;
    }
    return n;
}
