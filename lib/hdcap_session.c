/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * hdcap_session: the complete capture loop on a worker thread.
 *
 * open -> init -> select input -> wait for a stable signal -> per-input setup (TV standard,
 * HDMI/DVI colour and audio, analog timing) -> stream -> re-check the signal once a second.
 * Any failure, signal change, frame timeout or configuration change starts over. Frames, audio,
 * status and log messages are reported through callbacks (all called on the worker thread).
 */
#include "hdcap.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#define SIGNAL_CHECK_NS     1000000000ULL
#define NO_FRAME_TIMEOUT_NS 3000000000ULL
#define HDCP_CHECK_NS       250000000ULL
/* ADC auto-phase (VGA/Component). The phase found for a mode is remembered and set at once when the
 * mode comes back (a source that has just switched modes or cores often shows a boot or black
 * screen first, which measures wrong). A mode seen for the first time is measured before streaming.
 * Only modes whose sampling follows the source's pixel clock (family 0: PC/VESA, X68000) are
 * measured; they are checked again PHASE_RECHECK1/2 after the start, and every PHASE_RETRY_NS
 * while no peak has been found, for up to PHASE_RETRY_FOR_NS; a check only moves the phase when
 * the picture has at least half the detail it had when the phase was found. Broadcast-rate modes (240p, 480i, ...) sample at
 * 13.5 MHz whatever the source's pixel clock: no phase is right for them and the meter can favour
 * a flickering one, so they keep the input table's phase (as in v0.3.1) and are not measured. */
#define PHASE_MEMORY        64
#define PHASE_RECHECK1_NS   3000000000ULL
#define PHASE_RECHECK2_NS   10000000000ULL
#define PHASE_RETRY_NS      5000000000ULL
#define PHASE_RETRY_FOR_NS  60000000000ULL
/* Level readings: every PHASE_LEVEL_NS for PHASE_LEVEL_FOR_NS after a start, a phase-locked mode's
 * meter is read at the phase in use; at 1.5 times the detail the phase was found on, it is measured
 * again (each such sweep raises the bar, so a start sees at most a few). */
#define PHASE_LEVEL_NS      2000000000ULL
#define PHASE_LEVEL_FOR_NS  120000000000ULL

struct hdcap_session {
    hdcap_session_callbacks cb;
    pthread_t       thread;
    int             thread_running;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             stop;
    int             restart;           /* set when the configuration changes */
    int             picture_dirty;     /* only the SD picture settings changed */
    hdcap_session_config cfg;
    hdcap_session_status status;       /* guarded by lock */
    /* worker-only */
    uint64_t last_frame_ns, fps_window_ns, fps_frames;
    int      logged_w, logged_h, logged_interlaced; /* last frame geometry written to the log */
    uint64_t logged_ns;                             /* when it was written (rate limit) */
    unsigned unlogged_changes;                      /* size changes skipped by the rate limit */
    int      hold_frames;                           /* ADC phase being measured: don't deliver frames */
    /* ADC phase found for each analog mode (index into hdcap_modes()) on each analog input (VGA,
     * Component: different sources), reused when the mode comes back, and the sharpness the picture
     * had then */
    uint8_t  mem_valid[2][PHASE_MEMORY], mem_phase[2][PHASE_MEMORY];
    uint32_t mem_level[2][PHASE_MEMORY];
    int      last_w, last_h, last_interlaced;       /* previous frame's geometry */
    /* HDCP compliance (worker only): while the source encrypts, frames become a notice and audio
     * silence; checked before streaming starts and every HDCP_CHECK_NS while streaming */
    int      hdcp_blocked;
    uint8_t *notice;
    int      notice_w, notice_h;
};

uint64_t hdcap_now_ns(void)
{
#ifdef __APPLE__
    /* mach_absolute_time in ns: the host clock CoreMediaIO and Core Audio timestamps use */
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void logf_(hdcap_session *s, int level, const char *fmt, ...)
{
    if (!s->cb.log)
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s->cb.log(level, buf, s->cb.user);
}

static void set_status(hdcap_session *s, hdcap_session_state st, const hdcap_signal *sig, const char *detail)
{
    pthread_mutex_lock(&s->lock);
    s->status.state = st;
    s->status.input = s->cfg.input;
    if (sig)
        s->status.signal = *sig;
    else if (st != HDCAP_SESSION_STREAMING)
        memset(&s->status.signal, 0, sizeof s->status.signal);
    if (st != HDCAP_SESSION_STREAMING) {
        s->status.width = s->status.height = 0;
        s->status.fps = 0;
    }
    if (detail)
        snprintf(s->status.detail, sizeof s->status.detail, "%s", detail);
    if (st != HDCAP_SESSION_STREAMING)
        s->status.hdcp_blocked = 0;
    else if (s->status.hdcp_blocked)
        snprintf(s->status.detail, sizeof s->status.detail, "HDCP-protected source: capture blocked");
    hdcap_session_status copy = s->status;
    pthread_mutex_unlock(&s->lock);
    if (s->cb.on_status)
        s->cb.on_status(&copy, s->cb.user);
}

/* Wait up to ms, waking early on stop or restart. Returns 1 if the session should exit. */
static int wait_ms(hdcap_session *s, unsigned ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&s->lock);
    while (!s->stop && !s->restart) {
        if (pthread_cond_timedwait(&s->cond, &s->lock, &ts) != 0)
            break;
    }
    int stop = s->stop;
    pthread_mutex_unlock(&s->lock);
    return stop;
}

static void remember_phase(hdcap_session *s, int mem_in, int mode_idx, int phase, uint32_t level)
{
    if (mode_idx < 0 || mode_idx >= PHASE_MEMORY)
        return;
    s->mem_valid[mem_in][mode_idx] = 1;
    s->mem_phase[mem_in][mode_idx] = (uint8_t)phase;
    s->mem_level[mem_in][mode_idx] = level;
}

static int should_leave(hdcap_session *s)
{
    pthread_mutex_lock(&s->lock);
    int r = s->stop || s->restart;
    pthread_mutex_unlock(&s->lock);
    return r;
}

static void on_frame(const hdcap_frame *f, void *user)
{
    hdcap_session *s = user;
    uint64_t now = hdcap_now_ns();
    s->last_frame_ns = now;
    s->fps_frames++;
    /* Log frame size changes, at most one line per second so an unstable signal can't flood the
     * log: a size that changes again within the second is logged when the second is up, with the
     * number of changes in between. */
    int changed = f->width != s->last_w || f->height != s->last_h || f->interlaced != s->last_interlaced;
    if ((f->width != s->logged_w || f->height != s->logged_h || f->interlaced != s->logged_interlaced) &&
        (s->logged_ns == 0 || now - s->logged_ns >= 1000000000ULL)) {
        char extra[48] = "";
        if (s->unlogged_changes)
            snprintf(extra, sizeof extra, "; %u unlogged changes since", s->unlogged_changes);
        logf_(s, HDCAP_LOG_INFO, "frame size %dx%d%s (was %dx%d%s)", f->width, f->height, f->interlaced ? " interlaced" : "",
              s->logged_w, s->logged_h, extra);
        s->logged_w = f->width;
        s->logged_h = f->height;
        s->logged_interlaced = f->interlaced;
        s->logged_ns = now;
        s->unlogged_changes = 0;
    } else if (changed) {
        s->unlogged_changes++;
    }
    s->last_w = f->width;
    s->last_h = f->height;
    s->last_interlaced = f->interlaced;
    if (now - s->fps_window_ns >= 1000000000ULL) {
        pthread_mutex_lock(&s->lock);
        s->status.fps = s->fps_frames * 1e9 / (double)(now - s->fps_window_ns);
        s->status.width = f->width;
        s->status.height = f->height;
        s->status.interlaced = f->interlaced;
        pthread_mutex_unlock(&s->lock);
        s->fps_frames = 0;
        s->fps_window_ns = now;
    }
    if (s->hold_frames)
        return; /* the ADC phase is being measured: consumers keep the last frame */
    if (s->hdcp_blocked) {
        if (!s->notice || s->notice_w != f->width || s->notice_h != f->height) {
            free(s->notice);
            s->notice = malloc((size_t)f->width * f->height * 2);
            if (!s->notice)
                return;
            s->notice_w = f->width;
            s->notice_h = f->height;
            hdcap_render_hdcp_notice(s->notice, f->width, f->height);
        }
        hdcap_frame nf = { f->width, f->height, 0, f->index, s->notice };
        if (s->cb.on_frame)
            s->cb.on_frame(&nf, now, s->cb.user);
        return;
    }
    if (s->cb.on_frame)
        s->cb.on_frame(f, now, s->cb.user);
}

static void on_audio(const int16_t *samples, size_t count, void *user)
{
    hdcap_session *s = user;
    if (!s->cb.on_audio)
        return;
    if (s->hdcp_blocked) {   /* keep the audio timeline, deliver silence */
        static const int16_t zeros[512 * 2];
        uint64_t now = hdcap_now_ns();
        while (count) {
            size_t n = count > 512 ? 512 : count;
            s->cb.on_audio(zeros, n, now, s->cb.user);
            count -= n;
        }
        return;
    }
    s->cb.on_audio(samples, count, hdcap_now_ns(), s->cb.user);
}

/* Composite/S-Video picture settings from the current configuration. */
static void apply_picture(hdcap_session *s, hdcap *dev)
{
    static const hdcap_picture defaults = HDCAP_PICTURE_DEFAULTS;
    pthread_mutex_lock(&s->lock);
    hdcap_picture p = s->cfg.picture_set ? s->cfg.picture : defaults;
    s->picture_dirty = 0;
    pthread_mutex_unlock(&s->lock);
    int r = hdcap_set_picture(dev, &p);
    if (r < 0)
        logf_(s, HDCAP_LOG_WARNING, "picture settings: %s", hdcap_strerror(r));
}

/* Re-read the receiver's HDCP status and switch blocking on or off, logging transitions. */
static void hdcp_check(hdcap_session *s, hdcap *dev)
{
    uint8_t raw[2];
    int r = hdcap_hdcp_status(dev, raw);
    if (r < 0 || r == s->hdcp_blocked)
        return;
    s->hdcp_blocked = r;
    if (r)
        logf_(s, HDCAP_LOG_WARNING, "HDCP-protected source (receiver status %02x/%02x): capture blocked, showing notice",
              raw[0], raw[1]);
    else
        logf_(s, HDCAP_LOG_INFO, "HDCP no longer active (receiver status %02x/%02x): capture resumed", raw[0], raw[1]);
    pthread_mutex_lock(&s->lock);
    s->status.hdcp_blocked = r;
    if (r)
        snprintf(s->status.detail, sizeof s->status.detail, "HDCP-protected source: capture blocked");
    hdcap_session_status copy = s->status;
    pthread_mutex_unlock(&s->lock);
    if (s->cb.on_status)
        s->cb.on_status(&copy, s->cb.user);
}

static void *worker(void *data)
{
    hdcap_session *s = data;
#ifdef __APPLE__
    /* The capture loop must keep up with a 115-260 MB/s isochronous stream. Hosts such as
     * launchd-started daemons default to background QoS, which throttles CPU and I/O. */
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    while (1) {
        pthread_mutex_lock(&s->lock);
        if (s->stop) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        s->restart = 0;
        hdcap_session_config cfg = s->cfg;
        pthread_mutex_unlock(&s->lock);
        hdcap_input input = cfg.input;
        char detail[160];

        hdcap *dev = NULL;
        int r = hdcap_open(&dev);
        if (r < 0) {
            set_status(s, HDCAP_SESSION_NO_DEVICE, NULL, hdcap_strerror(r));
            if (wait_ms(s, 2000))
                break;
            continue;
        }
        if ((r = hdcap_init(dev)) < 0 || (r = hdcap_select_input(dev, input)) < 0) {
            logf_(s, HDCAP_LOG_WARNING, "select %s failed: %s", hdcap_input_name(input), hdcap_strerror(r));
            set_status(s, HDCAP_SESSION_ERROR, NULL, hdcap_strerror(r));
            hdcap_close(dev);
            if (wait_ms(s, 2000))
                break;
            continue;
        }
        int mbps = hdcap_link_speed_mbps(dev);
        pthread_mutex_lock(&s->lock);
        s->status.link_mbps = mbps;
        pthread_mutex_unlock(&s->lock);
        if (mbps < 5000)
            logf_(s, HDCAP_LOG_WARNING, "device is connected at %d Mbit/s, not USB 3 SuperSpeed: only Composite/S-Video "
                  "fit through USB 2.0. Use a USB 3 port, hub and cable.", mbps);
        logf_(s, HDCAP_LOG_INFO, "%s selected; waiting for signal", hdcap_input_name(input));
        set_status(s, HDCAP_SESSION_NO_SIGNAL, NULL, "waiting for signal");

        hdcap_signal sig;
        int locked = 0;
        while (!should_leave(s) && (locked = hdcap_wait_signal(dev, input, 1000, &sig)) == 0)
            ;
        if (locked <= 0) {
            if (locked < 0)
                logf_(s, HDCAP_LOG_WARNING, "signal read failed: %s", hdcap_strerror(locked));
            hdcap_close(dev);
            if (wait_ms(s, 500))
                break;
            continue;
        }
        char geom[64] = "";
        if (sig.hactive)
            snprintf(geom, sizeof geom, ", %d active / %d total pixels (%.2f MHz)", sig.hactive, sig.htotal,
                     sig.htotal * sig.hfreq_hz / 1e6);
        logf_(s, HDCAP_LOG_INFO, "signal: %s%s%d lines, %.3f kHz, %.2f Hz%s%s", sig.standard ? sig.standard : "",
              sig.standard ? ", " : "", sig.vtotal, sig.hfreq_hz / 1000, sig.vfreq_hz, sig.interlaced ? ", interlaced" : "", geom);
        snprintf(detail, sizeof detail, "%d lines, %.3f kHz, %.2f Hz%s", sig.vtotal, sig.hfreq_hz / 1000, sig.vfreq_hz,
                 sig.interlaced ? ", interlaced" : "");

        if (input == HDCAP_INPUT_COMPOSITE || input == HDCAP_INPUT_SVIDEO) {
            int st = hdcap_setup_sd(dev, cfg.tv_standard, 2000);
            apply_picture(s, dev);
            if (st >= 0) {
                logf_(s, HDCAP_LOG_INFO, "TV standard: %s%s", hdcap_std_name((hdcap_std)st),
                      cfg.tv_standard == HDCAP_STD_AUTO ? " (detected)" : " (forced)");
                snprintf(detail, sizeof detail, "%s", hdcap_std_name((hdcap_std)st));
            }
        }
        if (input == HDCAP_INPUT_HDMI || input == HDCAP_INPUT_DVI) {
            hdcap_digital_setup ds;
            if ((r = hdcap_setup_digital(dev, 3000, &ds)) < 0) {
                logf_(s, HDCAP_LOG_WARNING, "digital setup failed: %s", hdcap_strerror(r));
            } else {
                logf_(s, HDCAP_LOG_INFO, "digital: AVI %s, VIC %d, %s, %s audio", ds.avi.valid ? "present" : "absent",
                      ds.avi.vic, ds.ycbcr_passthru ? "YCbCr" : ds.full_range ? "RGB full range" : "RGB limited range",
                      ds.embedded_audio ? "embedded" : "line-in");
            }
        }
        /* HDCP before the first frame, so no protected frame is ever delivered */
        s->hdcp_blocked = 0;
        pthread_mutex_lock(&s->lock);
        s->status.hdcp_blocked = 0;
        pthread_mutex_unlock(&s->lock);
        int digital = input == HDCAP_INPUT_HDMI || input == HDCAP_INPUT_DVI;
        if (digital)
            hdcp_check(s, dev);
        const hdcap_mode *applied = NULL;
        int analog_phase = 0, phase_now = 0, phase_found = 0, mode_idx = -1;
        int mem_in = input == HDCAP_INPUT_COMPONENT;   /* phase memory: 0 = VGA, 1 = Component */
        uint32_t phase_level = 0;   /* sharpness when phase_now was found */
        if (input == HDCAP_INPUT_VGA || input == HDCAP_INPUT_COMPONENT) {
            size_t nm;
            const hdcap_mode *all = hdcap_modes(&nm);
            const hdcap_mode *m = (cfg.analog_mode >= 0 && (size_t)cfg.analog_mode < nm) ? &all[cfg.analog_mode]
                                                                                         : hdcap_find_mode(input, &sig);
            if (m) {
                logf_(s, HDCAP_LOG_INFO, "analog mode: %s%s, offset %+d/%+d", m->name, cfg.analog_mode >= 0 ? " (manual)" : "",
                      cfg.h_offset, cfg.v_offset);
                applied = m;
                if ((r = hdcap_set_mode(dev, input, m, cfg.h_offset, cfg.v_offset)) < 0) {
                    logf_(s, HDCAP_LOG_WARNING, "set mode failed: %s", hdcap_strerror(r));
                } else if (m->family == 0) {
                    /* Modes sampled at the source's pixel clock (PC/VESA, X68000): sample in the
                     * middle of the source's pixels; with the wrong phase, fine detail (1-pixel text
                     * strokes) flips between frames and shimmers. Broadcast-rate modes (240p, 480i,
                     * ...) keep the input table's phase, as before: they sample at 13.5 MHz whatever
                     * the source's pixel clock, and on undersampled detail (Amiga hires in 240p) the
                     * meter's peak was the phase that flickered most (1,192 changed pixels per frame,
                     * against 470-850 at the others). */
                    hdcap_phase_result ph;
                    mode_idx = (int)(m - all);
                    analog_phase = 1;
                    if (mode_idx < PHASE_MEMORY && s->mem_valid[mem_in][mode_idx]) {
                        phase_now = s->mem_phase[mem_in][mode_idx];
                        phase_level = s->mem_level[mem_in][mode_idx];
                        phase_found = 1;
                        if ((r = hdcap_set_phase(dev, phase_now)) < 0)
                            logf_(s, HDCAP_LOG_WARNING, "set ADC phase failed: %s", hdcap_strerror(r));
                        else
                            logf_(s, HDCAP_LOG_INFO, "ADC phase %d (remembered for this mode)", phase_now);
                    } else if ((r = hdcap_auto_phase(dev, &ph)) < 0) {
                        logf_(s, HDCAP_LOG_WARNING, "ADC auto-phase failed: %s", hdcap_strerror(r));
                    } else {
                        phase_now = ph.phase;
                        phase_found = ph.found;
                        if (ph.found) {
                            phase_level = ph.best;
                            remember_phase(s, mem_in, mode_idx, ph.phase, ph.best);
                            logf_(s, HDCAP_LOG_INFO, "ADC phase %d (sharpness %u, worst phase %u)", ph.phase, ph.best, ph.worst);
                        } else {
                            logf_(s, HDCAP_LOG_INFO, "ADC phase 0: %s (sharpness %u..%u); measuring again shortly",
                                  ph.unsteady ? "the picture changed while measuring" : "picture too flat to measure", ph.worst, ph.best);
                        }
                    }
                }
                snprintf(detail, sizeof detail, "%s", m->name);
            } else {
                logf_(s, HDCAP_LOG_WARNING, "no standard mode matches this signal; using default sampling");
            }
        }

        hdcap_signal locked_sig = sig;
        hdcap_demux *dx = hdcap_demux_new(on_frame, on_audio, s);
        if (dx && hdcap_wants_line_double(input, &sig)) {
            hdcap_demux_set_line_double(dx, 1);
            if (input == HDCAP_INPUT_VGA || input == HDCAP_INPUT_COMPONENT) {
                /* The output height follows the active lines the ADC window delivers (the "frame
                 * size" log line reports it), not a mode's nominal height: a forced 31 kHz mode on a
                 * 15 kHz source would otherwise be reported as e.g. 480p doubled to 960. */
                logf_(s, HDCAP_LOG_INFO, "progressive 15 kHz source (%d lines per frame): line-doubled", sig.vtotal);
            } else {
                int field_lines = sig.vtotal >= 300 ? 288 : 240;
                logf_(s, HDCAP_LOG_INFO, "progressive %dp source: line-doubled to %d lines", field_lines, 2 * field_lines);
            }
        }
        double rate = hdcap_estimate_rate(input, &sig, applied);
        logf_(s, HDCAP_LOG_INFO, "expected stream rate %.1f MB/s", rate / 1e6);
        if (!dx || (r = hdcap_stream_start_rate(dev, dx, rate)) < 0) {
            logf_(s, HDCAP_LOG_WARNING, "stream start failed: %s", hdcap_strerror(dx ? r : HDCAP_ERR_NOMEM));
            set_status(s, HDCAP_SESSION_ERROR, &sig, "stream start failed");
            hdcap_demux_free(dx);
            hdcap_close(dev);
            if (wait_ms(s, 2000))
                break;
            continue;
        }
        logf_(s, HDCAP_LOG_INFO, "streaming");
        s->last_frame_ns = s->fps_window_ns = hdcap_now_ns();
        s->fps_frames = 0;
        set_status(s, HDCAP_SESSION_STREAMING, &sig, detail);
        uint64_t next_check = hdcap_now_ns() + SIGNAL_CHECK_NS, next_hdcp = hdcap_now_ns() + HDCP_CHECK_NS;
        uint64_t stream_t0 = hdcap_now_ns(), next_phase = analog_phase ? stream_t0 + PHASE_RECHECK1_NS : 0;
        uint64_t next_level = analog_phase ? stream_t0 + PHASE_LEVEL_NS : 0;
        int phase_rechecks = 0, sweep_for_level = 0;
        hdcap_phase_sweep sweep;
        sweep.active = 0;
        while (!should_leave(s)) {
            if (digital && hdcap_now_ns() >= next_hdcp) {
                next_hdcp = hdcap_now_ns() + HDCP_CHECK_NS;
                hdcp_check(s, dev);
            }
            if (s->picture_dirty && (input == HDCAP_INPUT_COMPOSITE || input == HDCAP_INPUT_SVIDEO))
                apply_picture(s, dev);
            /* poll briefly while a phase sweep is measuring, so each 50 ms step is taken on time */
            if ((r = hdcap_stream_poll(dev, sweep.active ? 10 : 100)) < 0) {
                logf_(s, HDCAP_LOG_WARNING, "USB error: %s", hdcap_strerror(r));
                break;
            }
            uint64_t now = hdcap_now_ns();
            /* While the phase is being changed the picture's sampling changes: frames are held
             * back (consumers keep showing the last one) until it is restored. A re-check is a
             * quick look at the phase's neighbours; only a phase that is off its peak, or no peak
             * found yet, gets a full sweep. In between, level readings (which don't change the
             * phase or the picture) watch for a picture with much more detail than the one the
             * phase was found on, e.g. a desktop arriving after a boot screen, and re-measure. */
            if (!sweep.active && next_phase && now >= next_phase) {
                r = phase_found ? hdcap_phase_check_start(dev, &sweep, phase_now, now) : hdcap_phase_sweep_start(dev, &sweep, now);
                if (r < 0) {
                    logf_(s, HDCAP_LOG_WARNING, "ADC phase re-check failed: %s", hdcap_strerror(r));
                    next_phase = 0;
                }
                s->hold_frames = sweep.active;
                sweep_for_level = 0;
            } else if (!sweep.active && next_level && now >= next_level && phase_found) {
                if (hdcap_phase_level_start(dev, &sweep, phase_now, now) < 0)
                    next_level = 0;
            } else if (sweep.active && (r = hdcap_phase_sweep_poll(dev, &sweep, now)) != 0) {
                if (r > 0 && sweep.check == 2) {
                    uint32_t level = sweep.res.fine[2];
                    next_level = now - stream_t0 < PHASE_LEVEL_FOR_NS ? now + PHASE_LEVEL_NS : 0;
                    if ((uint64_t)level * 2 >= (uint64_t)phase_level * 3) {
                        logf_(s, HDCAP_LOG_INFO, "ADC phase %d measured again at %.1f s: the picture has more detail than "
                              "when it was found (sharpness %u, then %u)", phase_now, (now - stream_t0) / 1e9, level, phase_level);
                        if (hdcap_phase_sweep_start(dev, &sweep, now) == 0) {
                            s->hold_frames = 1;
                            sweep_for_level = 1;
                        }
                    }
                    continue;
                }
                if (r > 0 && sweep.check) {
                    const hdcap_phase_result *ph = &sweep.res;
                    logf_(s, HDCAP_LOG_INFO, "ADC phase check at %.1f s: %d %s (sharpness %u, neighbours %u/%u)",
                          (now - stream_t0) / 1e9, ph->phase, ph->check_ok ? "holds" : "is off its peak", ph->fine[2], ph->fine[0],
                          ph->fine[1]);
                    if (!ph->check_ok && (uint64_t)ph->fine[2] * 2 < phase_level) {
                        /* e.g. a boot screen before the picture the phase was found on: its peak
                         * says little about that picture, so keep the phase */
                        logf_(s, HDCAP_LOG_INFO, "ADC phase %d kept: the picture has less detail than when it was found "
                              "(sharpness %u, then %u)", ph->phase, ph->fine[2], phase_level);
                    } else if (!ph->check_ok && (r = hdcap_phase_sweep_start(dev, &sweep, now)) == 0) {
                        continue;   /* full sweep, frames still held */
                    }
                }
                s->hold_frames = 0;
                if (!sweep_for_level)
                    phase_rechecks++;
                if (r < 0) {
                    /* the sweep stopped part-way: don't leave the picture at a phase being measured */
                    hdcap_set_phase(dev, phase_now);
                    logf_(s, HDCAP_LOG_WARNING, "ADC phase re-check failed: %s", hdcap_strerror(r));
                    next_phase = 0;
                } else if (sweep.check) {
                    next_phase = phase_rechecks == 1 ? stream_t0 + PHASE_RECHECK2_NS : 0;
                } else {
                    const hdcap_phase_result *ph = &sweep.res;
                    logf_(s, HDCAP_LOG_INFO, "ADC phase re-check at %.1f s: %s%d (sharpness %u..%u)", (now - stream_t0) / 1e9,
                          ph->unsteady ? "the picture changed while measuring, keeping " : ph->found ? "best " : "no peak, keeping ",
                          ph->found ? ph->phase : phase_now, ph->worst, ph->best);
                    if (ph->found && (!phase_found || ph->phase != phase_now))
                        logf_(s, HDCAP_LOG_INFO, "ADC phase re-check: %d -> %d (sharpness %u, worst phase %u)", phase_now,
                              ph->phase, ph->best, ph->worst);
                    else if (!ph->found && phase_found)
                        /* the picture went flat (e.g. a black screen): keep the phase found earlier */
                        hdcap_set_phase(dev, phase_now);
                    if (ph->found) {
                        phase_now = ph->phase;
                        phase_found = 1;
                        phase_level = ph->best;
                        remember_phase(s, mem_in, mode_idx, ph->phase, ph->best);
                    }
                    if (!sweep_for_level) {
                        if (!phase_found && now - stream_t0 < PHASE_RETRY_FOR_NS)
                            next_phase = now + PHASE_RETRY_NS;
                        else if (phase_rechecks == 1)
                            next_phase = stream_t0 + PHASE_RECHECK2_NS;
                        else
                            next_phase = 0;
                    }
                }
                sweep_for_level = 0;
            }
            if (now >= next_check) {
                next_check = now + SIGNAL_CHECK_NS;
                hdcap_signal cur;
                if (hdcap_get_signal(dev, input, &cur) == 0 &&
                    (!cur.locked || abs(cur.vtotal - locked_sig.vtotal) > 3 || cur.interlaced != locked_sig.interlaced ||
                     fabs(cur.hfreq_hz - locked_sig.hfreq_hz) > locked_sig.hfreq_hz * 0.01 ||
                     (cur.hactive && locked_sig.hactive && abs(cur.hactive - locked_sig.hactive) > 8))) {
                    logf_(s, HDCAP_LOG_INFO, "signal changed (%d lines, %.3f kHz); restarting", cur.vtotal, cur.hfreq_hz / 1000);
                    break;
                }
                if (s->cb.on_status) {
                    pthread_mutex_lock(&s->lock);
                    hdcap_session_status copy = s->status;
                    pthread_mutex_unlock(&s->lock);
                    s->cb.on_status(&copy, s->cb.user);
                }
            }
            /* Re-read the clock: the signal check above does control transfers, during which libusb
             * also completes isochronous transfers, so a frame may have arrived after `now`. */
            now = hdcap_now_ns();
            if (now > s->last_frame_ns && now - s->last_frame_ns > NO_FRAME_TIMEOUT_NS) {
                hdcap_stream_stats us;
                hdcap_demux_stats ds;
                hdcap_stream_get_stats(dev, &us);
                hdcap_demux_get_stats(dx, &ds);
                logf_(s, HDCAP_LOG_INFO, "no frames for 3 s; restarting (usb: %llu transfers, %llu packets, %llu empty, %llu errors, "
                      "%llu bytes; demux: %llu lines, %llu frames, %llu dropped, %llu resyncs, last line %d B)",
                      (unsigned long long)us.transfers, (unsigned long long)us.packets, (unsigned long long)us.empty_packets,
                      (unsigned long long)us.error_packets, (unsigned long long)us.bytes, (unsigned long long)ds.video_lines,
                      (unsigned long long)ds.frames, (unsigned long long)ds.dropped_frames, (unsigned long long)ds.resyncs,
                      ds.last_line_bytes);
                break;
            }
        }
        hdcap_phase_sweep_abort(dev, &sweep);
        s->hold_frames = 0;
        hdcap_stream_stop(dev);
        hdcap_demux_free(dx);
        hdcap_close(dev);
        set_status(s, HDCAP_SESSION_NO_SIGNAL, NULL, "restarting");
        if (wait_ms(s, 500))
            break;
    }
    set_status(s, HDCAP_SESSION_STOPPED, NULL, "stopped");
    return NULL;
}

hdcap_session *hdcap_session_start(const hdcap_session_config *cfg, const hdcap_session_callbacks *cb)
{
    hdcap_session *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->cfg = *cfg;
    if (cb)
        s->cb = *cb;
    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond, NULL);
    if (pthread_create(&s->thread, NULL, worker, s) != 0) {
        pthread_cond_destroy(&s->cond);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    s->thread_running = 1;
    return s;
}

static int same_except_picture(const hdcap_session_config *a, const hdcap_session_config *b)
{
    return a->input == b->input && a->analog_mode == b->analog_mode && a->h_offset == b->h_offset &&
           a->v_offset == b->v_offset && a->tv_standard == b->tv_standard;
}

void hdcap_session_set_config(hdcap_session *s, const hdcap_session_config *cfg)
{
    pthread_mutex_lock(&s->lock);
    int picture_only = same_except_picture(&s->cfg, cfg);
    s->cfg = *cfg;
    if (picture_only) {
        s->picture_dirty = 1;   /* applied by the worker while streaming, no restart */
        pthread_mutex_unlock(&s->lock);
        return;
    }
    s->restart = 1;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

void hdcap_session_get_config(hdcap_session *s, hdcap_session_config *cfg)
{
    pthread_mutex_lock(&s->lock);
    *cfg = s->cfg;
    pthread_mutex_unlock(&s->lock);
}

void hdcap_session_get_status(hdcap_session *s, hdcap_session_status *st)
{
    pthread_mutex_lock(&s->lock);
    *st = s->status;
    pthread_mutex_unlock(&s->lock);
}

void hdcap_session_stop(hdcap_session *s)
{
    if (!s)
        return;
    pthread_mutex_lock(&s->lock);
    s->stop = 1;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    if (s->thread_running)
        pthread_join(s->thread, NULL);
    pthread_cond_destroy(&s->cond);
    pthread_mutex_destroy(&s->lock);
    free(s->notice);
    free(s);
}

const char *hdcap_session_state_name(hdcap_session_state st)
{
    switch (st) {
    case HDCAP_SESSION_STARTING:  return "starting";
    case HDCAP_SESSION_NO_DEVICE: return "no device";
    case HDCAP_SESSION_NO_SIGNAL: return "no signal";
    case HDCAP_SESSION_STREAMING: return "streaming";
    case HDCAP_SESSION_ERROR:     return "error";
    case HDCAP_SESSION_STOPPED:   return "stopped";
    }
    return "?";
}
