/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * hdcap-capture: run init sequences, stream from the USB3HDCAP, save frames (PPM) and audio (WAV).
 *
 *   hdcap-capture -s sequences/open.seq -s sequences/select_hdmi.seq -t 5 -n 3 -o out/
 */
#include "hdcap.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    const char *outdir;
    int   want_frames, saved;
    uint8_t *rgb;
    size_t   rgb_size;
    FILE    *wav;
    uint64_t wav_frames;
    int last_w, last_h, last_i;
    int hdcp_blocked;   /* HDCP-protected source: frames are saved as the notice, audio as silence */
    uint8_t *notice;
    int notice_w, notice_h;
} ctx_t;

static volatile sig_atomic_t stop;
static void on_sigint(int s) { (void)s; stop = 1; }

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void on_frame(const hdcap_frame *f, void *user)
{
    ctx_t *c = user;
    c->last_w = f->width;
    c->last_h = f->height;
    c->last_i = f->interlaced;
    if (c->saved >= c->want_frames)
        return;
    size_t need = (size_t)f->width * f->height * 3;
    if (need > c->rgb_size) {
        free(c->rgb);
        c->rgb = malloc(need);
        c->rgb_size = c->rgb ? need : 0;
        if (!c->rgb)
            return;
    }
    const uint8_t *src = f->yuyv;
    if (c->hdcp_blocked) {
        if (!c->notice || c->notice_w != f->width || c->notice_h != f->height) {
            free(c->notice);
            c->notice = malloc((size_t)f->width * f->height * 2);
            if (!c->notice)
                return;
            c->notice_w = f->width;
            c->notice_h = f->height;
            hdcap_render_hdcp_notice(c->notice, f->width, f->height);
        }
        src = c->notice;
    }
    hdcap_yuyv_to_rgb24(src, c->rgb, f->width, f->height);
    char path[1024];
    snprintf(path, sizeof path, "%s/frame_%03d.ppm", c->outdir, c->saved);
    FILE *o = fopen(path, "wb");
    if (!o) {
        perror(path);
        return;
    }
    fprintf(o, "P6\n%d %d\n255\n", f->width, f->height);
    fwrite(c->rgb, 1, need, o);
    fclose(o);
    fprintf(stderr, "saved %s (%dx%d%s)\n", path, f->width, f->height, f->interlaced ? ", woven fields" : "");
    c->saved++;
}

static void on_audio(const int16_t *s, size_t n, void *user)
{
    ctx_t *c = user;
    if (c->wav && c->hdcp_blocked) {
        static const int16_t zeros[512 * 2];
        for (size_t k = 0; k < n; k += 512)
            fwrite(zeros, 4, n - k > 512 ? 512 : n - k, c->wav);
        c->wav_frames += n;
    } else if (c->wav) {
        fwrite(s, 4, n, c->wav);
        c->wav_frames += n;
    }
}

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t data = frames * 4, riff = 36 + data, fmt = 16, rate = 48000, brate = 192000;
    uint16_t pcm = 1, ch = 2, align = 4, bits = 16;
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f); fwrite(&rate, 4, 1, f);
    fwrite(&brate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
}

static void usage(void)
{
    fprintf(stderr,
            "usage: hdcap-capture [-s seqfile]... [-t seconds] [-n frames] [-o outdir] [-v]\n"
            "  -i INPUT  init and select input: hdmi, dvi, vga, component, composite, s-video\n"
            "  -m NAME   analog mode override (substring of a mode name, e.g. \"27 MHz\", \"800x600@60\")\n"
            "  -T HT,HA,HS,VA,VS  analog custom timing (overrides -m): samples per line, window width,\n"
            "            window start (sync + back porch), window height (lines per frame), first line;\n"
            "            the pixel clock follows from the measured line rate. For finding a new mode's values\n"
            "  -H N, -V N  analog window offset in samples / lines\n"
            "  -p N      analog ADC sampling phase 0..63 (default: automatic from the chip's sharpness meter\n"
            "            for modes sampled at the source's pixel clock, the input table's for broadcast-rate modes)\n"
            "  -S STD    TV standard for composite/S-Video: Auto (default), NTSC-M, PAL, SECAM, NTSC-4.43, PAL-M, PAL-CN, PAL-60\n"
            "  -P B,C,S,H,SH  composite/S-Video picture: brightness, contrast %%, saturation (128 = 100%%), hue, sharpness\n"
            "  -s FILE   run sequence file before streaming (repeatable, in order)\n"
            "  -t SEC    stream duration (default 5)\n"
            "  -n N      number of frames to save as PPM (default 3)\n"
            "  -o DIR    output directory (default capture-out)\n"
            "  -D MS     cap every sequence delay at MS milliseconds\n"
            "  -v        verbose (repeat for per-operation trace)\n");
}

int main(int argc, char **argv)
{
    const char *seqs[16];
    int nseq = 0, verbose = 0, max_delay = -1, input = -1, hoff = 0, voff = 0;
    double rate = 1.0;
    const char *mode_name = NULL;
    int custom[5], have_custom = 0, phase = -1;
    hdcap_mode custom_mode;
    int tv_std = HDCAP_STD_AUTO;
    hdcap_picture pic = HDCAP_PICTURE_DEFAULTS;
    int pic_set = 0;
    double seconds = 5;
    ctx_t c = { .outdir = "capture-out", .want_frames = 3 };
    int opt;
    while ((opt = getopt(argc, argv, "s:i:m:T:H:V:p:S:P:t:n:o:D:vh")) != -1) {
        switch (opt) {
        case 's': if (nseq < 16) seqs[nseq++] = optarg; break;
        case 't': seconds = atof(optarg); break;
        case 'n': c.want_frames = atoi(optarg); break;
        case 'o': c.outdir = optarg; break;
        case 'v': verbose++; break;
        case 'D': max_delay = atoi(optarg); break;
        case 'm': mode_name = optarg; break;
        case 'T':
            if (sscanf(optarg, "%d,%d,%d,%d,%d", &custom[0], &custom[1], &custom[2], &custom[3], &custom[4]) != 5 ||
                custom[0] < 64 || custom[0] > 4096 || custom[1] < 16 || custom[1] > 4095 || custom[2] < 1 ||
                custom[3] < 16 || custom[3] > 4095 || custom[4] < 0 || custom[4] > 255) {
                usage();
                return 2;
            }
            have_custom = 1;
            break;
        case 'P':
            if (sscanf(optarg, "%d,%d,%d,%d,%d", &pic.brightness, &pic.contrast, &pic.saturation, &pic.hue,
                       &pic.sharpness) != 5) { usage(); return 2; }
            pic_set = 1;
            break;
        case 'S':
            for (int k = 0; k <= HDCAP_STD_AUTO; k++)
                if (!strcasecmp(optarg, hdcap_std_name(k)))
                    tv_std = k;
            break;
        case 'p':
            phase = atoi(optarg);
            if (phase < 0 || phase > 63) { usage(); return 2; }
            break;
        case 'H': hoff = atoi(optarg); break;
        case 'V': voff = atoi(optarg); break;
        case 'i':
            for (int k = 0; k < HDCAP_INPUT_COUNT; k++)
                if (!strcasecmp(optarg, hdcap_input_name(k)) || (k == HDCAP_INPUT_SVIDEO && !strcasecmp(optarg, "svideo")))
                    input = k;
            if (input < 0) { usage(); return 2; }
            break;
        default: usage(); return 2;
        }
    }
    if (mkdir(c.outdir, 0755) < 0 && errno != EEXIST) {
        perror(c.outdir);
        return 1;
    }

    hdcap *dev;
    int r = hdcap_open(&dev);
    if (r < 0) {
        fprintf(stderr, "open: %s\n", hdcap_strerror(r));
        return 1;
    }
    int mbps = hdcap_link_speed_mbps(dev);
    fprintf(stderr, "USB link: %d Mbit/s%s\n", mbps,
            mbps < 5000 ? "  (WARNING: not SuperSpeed; only SD composite/S-Video can fit through USB 2.0)" : "");
    hdcap_signal sig = {0};
    int have_sig = 0;
    if (input >= 0) {
        double t0 = now();
        if ((r = hdcap_init(dev)) < 0 || (r = hdcap_select_input(dev, input)) < 0) {
            fprintf(stderr, "init/select %s: %s\n", hdcap_input_name(input), hdcap_strerror(r));
            hdcap_close(dev);
            return 1;
        }
        fprintf(stderr, "selected %s in %.2f s; waiting for signal\n", hdcap_input_name(input), now() - t0);
        r = hdcap_wait_signal(dev, input, 5000, &sig);
        have_sig = r == 1;
        if (r < 0) {
            fprintf(stderr, "signal: %s\n", hdcap_strerror(r));
            hdcap_close(dev);
            return 1;
        }
        char geom[64] = "";
        if (sig.hactive)
            snprintf(geom, sizeof geom, ", %d active / %d total (%.2f MHz)", sig.hactive, sig.htotal,
                     sig.htotal * sig.hfreq_hz / 1e6);
        fprintf(stderr, "signal %s: %s%s%d lines, %.3f kHz, %.2f Hz%s%s  [raw %02x %02x %02x %02x %02x %02x %02x %02x]\n",
                r ? "locked" : "NOT locked", sig.standard ? sig.standard : "", sig.standard ? " " : "",
                sig.vtotal, sig.hfreq_hz / 1000, sig.vfreq_hz, sig.interlaced ? " interlaced" : "", geom,
                sig.raw[0], sig.raw[1], sig.raw[2], sig.raw[3], sig.raw[4], sig.raw[5], sig.raw[6], sig.raw[7]);
        if (r == 1 && (input == HDCAP_INPUT_COMPOSITE || input == HDCAP_INPUT_SVIDEO)) {
            int st = hdcap_setup_sd(dev, tv_std, 2000);
            if (st < 0)
                fprintf(stderr, "TV standard: %s\n", hdcap_strerror(st));
            else
                fprintf(stderr, "TV standard: %s%s\n", hdcap_std_name(st), tv_std == HDCAP_STD_AUTO ? " (detected)" : " (forced)");
            if (pic_set && (r = hdcap_set_picture(dev, &pic)) < 0)
                fprintf(stderr, "picture: %s\n", hdcap_strerror(r));
            else if (pic_set)
                fprintf(stderr, "picture: brightness %d, contrast %d%%, saturation %d, hue %d, sharpness %d\n", pic.brightness,
                        pic.contrast, pic.saturation, pic.hue, pic.sharpness);
        }
        rate = hdcap_estimate_rate(input, &sig, NULL);
        if (r == 1 && (input == HDCAP_INPUT_HDMI || input == HDCAP_INPUT_DVI)) {
            hdcap_digital_setup ds;
            int rr = hdcap_setup_digital(dev, 3000, &ds);
            if (rr < 0)
                fprintf(stderr, "digital setup: %s\n", hdcap_strerror(rr));
            else
                fprintf(stderr, "digital: AVI %s [%02x %02x %02x %02x] VIC %d -> %s, %s audio\n",
                        ds.avi.valid ? "yes" : "none", ds.avi.pb[0], ds.avi.pb[1], ds.avi.pb[2], ds.avi.pb[3], ds.avi.vic,
                        ds.ycbcr_passthru ? "YCbCr pass-through" : ds.full_range ? (ds.bt709 ? "RGB full-range BT.709" : "RGB full-range BT.601")
                                                                   : (ds.bt709 ? "RGB limited BT.709" : "RGB limited BT.601"),
                        ds.embedded_audio ? "embedded" : "line-in");
        }
        if (input == HDCAP_INPUT_VGA || input == HDCAP_INPUT_COMPONENT) {
            const hdcap_mode *m = NULL;
            size_t nm;
            const hdcap_mode *all = hdcap_modes(&nm);
            if (have_custom && r == 1) {
                custom_mode = (hdcap_mode){ "custom (-T)", custom[0], custom[1], custom[2], sig.vtotal, custom[3], custom[4],
                                            custom[0] * sig.hfreq_hz / 1e6, sig.interlaced, 0 };
                m = &custom_mode;
                fprintf(stderr, "custom timing: %d samples per line at %.3f kHz = %.2f MHz\n", custom[0], sig.hfreq_hz / 1000,
                        custom_mode.pclk_mhz);
            } else if (have_custom) {
                fprintf(stderr, "custom timing needs a locked signal\n");
            } else if (mode_name) {
                for (size_t k = 0; k < nm && !m; k++)
                    if (strstr(all[k].name, mode_name))
                        m = &all[k];
                if (!m)
                    fprintf(stderr, "no mode matching \"%s\"\n", mode_name);
            } else if (r == 1) {
                m = hdcap_find_mode(input, &sig);
            }
            rate = hdcap_estimate_rate(input, &sig, m);
            if (m) {
                fprintf(stderr, "mode: %s (h-total %d, active %dx%d%s, window start %d/%d, offset %+d/%+d)\n", m->name,
                        m->htotal, m->hactive, m->vactive, m->interlaced ? " per field" : "", m->hstart, m->vstart, hoff, voff);
                if ((r = hdcap_set_mode(dev, input, m, hoff, voff)) < 0) {
                    fprintf(stderr, "set mode: %s\n", hdcap_strerror(r));
                } else if (phase >= 0) {
                    if ((r = hdcap_set_phase(dev, phase)) < 0)
                        fprintf(stderr, "set phase: %s\n", hdcap_strerror(r));
                    else
                        fprintf(stderr, "ADC phase %d (forced)\n", phase);
                } else if (m->family != 0) {
                    /* as the session: broadcast-rate modes keep the input table's phase */
                    fprintf(stderr, "ADC phase: input table's (broadcast-rate mode; -p to force one)\n");
                } else {
                    hdcap_phase_result ph;
                    if ((r = hdcap_auto_phase(dev, &ph)) < 0) {
                        fprintf(stderr, "auto-phase: %s\n", hdcap_strerror(r));
                    } else {
                        fprintf(stderr, "ADC phase %d (%s; coarse peak %d; meter", ph.phase,
                                ph.found ? "auto" : ph.unsteady ? "picture changed while measuring, default" : "picture too flat, default",
                                ph.coarse_phase);
                        for (int k = 0; k < HDCAP_PHASE_STEPS; k++)
                            fprintf(stderr, " %u", ph.value[k]);
                        if (ph.found) {
                            fprintf(stderr, "; fine");
                            for (int k = 0; k < HDCAP_PHASE_FINE; k++)
                                fprintf(stderr, " %u", ph.fine[k]);
                        }
                        fprintf(stderr, ")\n");
                    }
                }
            } else {
                fprintf(stderr, "mode: none matched; keeping the input table's default sampling\n");
            }
        }
    }
    for (int i = 0; i < nseq; i++) {
        fprintf(stderr, "running %s\n", seqs[i]);
        double t0 = now();
        if ((r = hdcap_run_sequence_ex(dev, seqs[i], verbose, max_delay)) < 0) {
            hdcap_close(dev);
            return 1;
        }
        fprintf(stderr, "  done in %.2f s\n", now() - t0);
    }

    char wavpath[1024];
    snprintf(wavpath, sizeof wavpath, "%s/audio.wav", c.outdir);
    c.wav = fopen(wavpath, "wb");
    if (c.wav)
        wav_header(c.wav, 0);

    /* HDCP compliance: checked before the first frame and every 250 ms while streaming */
    int digital = input == HDCAP_INPUT_HDMI || input == HDCAP_INPUT_DVI;
    uint8_t hraw[2];
    if (digital && hdcap_hdcp_status(dev, hraw) == 1) {
        c.hdcp_blocked = 1;
        fprintf(stderr, "HDCP-protected source (receiver status %02x/%02x): capture blocked\n", hraw[0], hraw[1]);
    }
    hdcap_demux *dx = hdcap_demux_new(on_frame, on_audio, &c);
    if (dx && have_sig && hdcap_wants_line_double(input, &sig)) {
        hdcap_demux_set_line_double(dx, 1);
        fprintf(stderr, "progressive SD source (%d lines): line doubling\n", sig.vtotal);
    }
    fprintf(stderr, "expected stream rate: %.1f MB/s\n", rate / 1e6);
    if (!dx || (r = hdcap_stream_start_rate(dev, dx, rate)) < 0) {
        fprintf(stderr, "stream start: %s\n", hdcap_strerror(dx ? r : HDCAP_ERR_NOMEM));
        hdcap_demux_free(dx);
        hdcap_close(dev);
        return 1;
    }
    signal(SIGINT, on_sigint);
    double t0 = now(), next = t0 + 1, next_hdcp = t0 + 0.25;
    uint64_t last_frames = 0, last_bytes = 0;
    while (!stop && now() - t0 < seconds) {
        hdcap_stream_poll(dev, 100);
        if (digital && now() >= next_hdcp) {
            next_hdcp = now() + 0.25;
            int h = hdcap_hdcp_status(dev, hraw);
            if (h >= 0 && h != c.hdcp_blocked) {
                c.hdcp_blocked = h;
                fprintf(stderr, "%s (receiver status %02x/%02x)\n", h ? "HDCP-protected source: capture blocked"
                                                                       : "HDCP no longer active: capture resumed", hraw[0], hraw[1]);
            }
        }
        if (now() >= next) {
            hdcap_demux_stats ds;
            hdcap_stream_stats ss;
            hdcap_demux_get_stats(dx, &ds);
            hdcap_stream_get_stats(dev, &ss);
            fprintf(stderr, "%5.1fs  xfers %llu pkts %llu empty %llu  %6.1f MB/s  %3llu fps  frame %dx%d%s  line %d B  audio %llu  resync %llu  drop %llu  pkt err %llu\n",
                    now() - t0, (unsigned long long)ss.transfers, (unsigned long long)ss.packets, (unsigned long long)ss.empty_packets, (ss.bytes - last_bytes) / 1e6, (unsigned long long)(ds.frames - last_frames),
                    c.last_w, c.last_h, c.last_i ? "i" : "p", ds.last_line_bytes,
                    (unsigned long long)ds.audio_frames, (unsigned long long)ds.resyncs,
                    (unsigned long long)ds.dropped_frames, (unsigned long long)ss.error_packets);
            last_frames = ds.frames;
            last_bytes = ss.bytes;
            next += 1;
        }
    }
    hdcap_stream_stop(dev);
    if (c.wav) {
        wav_header(c.wav, (uint32_t)c.wav_frames);
        fclose(c.wav);
        fprintf(stderr, "saved %s (%.1f s)\n", wavpath, c.wav_frames / 48000.0);
    }
    hdcap_demux_free(dx);
    hdcap_close(dev);
    free(c.rgb);
    free(c.notice);
    return 0;
}
