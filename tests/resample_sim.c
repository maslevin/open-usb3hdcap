/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * resample_sim: drive hdcap_resampler with a simulated card (clock offset in ppm, bursty jittery
 * USB arrival, 1 kHz tone) and check the output timeline against the true capture time,
 * waveform continuity (second-difference spikes) and gap handling.
 *   resample_sim [PPM] [MINUTES] [GAP_AT_MINUTE]
 */
#include "hdcap.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    double ppm = argc > 1 ? atof(argv[1]) : -218, minutes = argc > 2 ? atof(argv[2]) : 30;
    double gap_at = argc > 3 ? atof(argv[3]) * 60 : -1;
    double card = 48000.0 * (1 + ppm * 1e-6), f = 1000, A = 16000;
    uint64_t in_n = (uint64_t)card; /* card stream starts at t = 1 s */
    hdcap_resampler *r = hdcap_resampler_new();
    srand(1);
    uint64_t out_n = 0;
    double t = 1.0, max_err = 0, max_d2 = 0, sum_err = 0;
    double limit_d2 = A * pow(2 * M_PI * f / 48000, 2) * 1.1 + 4;
    long spikes = 0, checks = 0;
    int16_t in[4096 * 2], out[4200 * 2], prev[2][2] = {{0}};
    int gapped = 0;
    double next_report = 60;
    while (t < minutes * 60) {
        /* one USB burst: whatever the card produced since the last burst, arriving 0..3 ms late */
        double burst_end = t;
        uint64_t avail = (uint64_t)(burst_end * card) - in_n;
        if (gap_at > 0 && !gapped && t > gap_at) { /* signal loss: 2 s of nothing */
            in_n += (uint64_t)(2 * card); t += 2; gapped = 1; continue;
        }
        if (avail > 4096) avail = 4096;
        for (uint64_t i = 0; i < avail; i++) {
            double ct = (in_n + i) / card;           /* true capture time */
            int16_t v = (int16_t)lrint(A * sin(2 * M_PI * f * ct));
            in[i * 2] = v;                           /* L: 1 kHz tone (continuity) */
            in[i * 2 + 1] = (int16_t)lrint(A * sin(2 * M_PI * 0.5 * ct));  /* R: 0.5 Hz (timing) */
        }
        double arrive = burst_end + (rand() % 3000) * 1e-6;
        uint64_t ts;
        size_t n = hdcap_resampler_process(r, in, avail, (uint64_t)(arrive * 1e9), out, 4200, &ts);
        /* check each output frame: its stamp vs true capture time of the sample it carries
         * (phase of the tone): v = A sin(2 pi f t_true) -> compare with stamp time */
        for (size_t i = 0; i < n; i++) {
            double stamp = ts / 1e9 + i / 48000.0;
            /* continuity */
            double d2 = fabs(out[i * 2] - 2.0 * prev[1][0] + prev[0][0]);
            if (out_n + i > 2 && d2 > max_d2) max_d2 = d2;
            if (out_n + i > 2 && d2 > limit_d2) spikes++;
            prev[0][0] = prev[1][0]; prev[1][0] = out[i * 2];
            /* timeline error: upward zero crossings of the 0.5 Hz channel happen at true times k*2 s */
            if (i > 0 && out[(i - 1) * 2 + 1] < 0 && out[i * 2 + 1] >= 0) {
                double frac = -out[(i - 1) * 2 + 1] / (double)(out[i * 2 + 1] - out[(i - 1) * 2 + 1]);
                double t_stamp = stamp - (1 - frac) / 48000.0;
                double t_true = round(t_stamp / 2) * 2;
                double e = (t_stamp - t_true) * 1e3;
                if (fabs(e) > max_err && stamp > 30) max_err = fabs(e);
                if (stamp > 30) { sum_err += e; checks++; }
            }
        }
        out_n += n;
        in_n += avail;
        t += 0.004 + ((rand() % 2000) - 1000) * 1e-6;
        if (t >= next_report) {
            hdcap_resampler_stats st;
            hdcap_resampler_get_stats(r, &st);
            if ((int)next_report % 300 == 0 || next_report == 60)
                printf("%5.0fs  card %.3f Hz  step %+.1f ppm  err %+.3f ms  out/in-time %.6f  resets %llu\n", t,
                       st.card_rate_hz, st.ratio_ppm, st.error_ms, out_n / 48000.0 / t, (unsigned long long)st.resets);
            next_report += 60;
        }
    }
    hdcap_resampler_stats st;
    hdcap_resampler_get_stats(r, &st);
    printf("timeline vs true capture time (after 30 s, from the 0.5 Hz channel): max |err| %.3f ms, mean %.3f ms\n",
           max_err, sum_err / checks);
    printf("continuity: max second difference %.0f (clean tone limit %.0f), spikes %ld, resets %llu\n", max_d2, limit_d2,
           spikes, (unsigned long long)st.resets);
    hdcap_resampler_free(r);
    return 0;
}
