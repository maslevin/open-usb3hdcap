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

struct hdcap_session {
    hdcap_session_callbacks cb;
    pthread_t       thread;
    int             thread_running;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    int             stop;
    int             restart;           /* set when the configuration changes */
    hdcap_session_config cfg;
    hdcap_session_status status;       /* guarded by lock */
    /* worker-only */
    uint64_t last_frame_ns, fps_window_ns, fps_frames;
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
    if (s->cb.on_frame)
        s->cb.on_frame(f, now, s->cb.user);
}

static void on_audio(const int16_t *samples, size_t count, void *user)
{
    hdcap_session *s = user;
    if (s->cb.on_audio)
        s->cb.on_audio(samples, count, hdcap_now_ns(), s->cb.user);
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
        const hdcap_mode *applied = NULL;
        if (input == HDCAP_INPUT_VGA || input == HDCAP_INPUT_COMPONENT) {
            size_t nm;
            const hdcap_mode *all = hdcap_modes(&nm);
            const hdcap_mode *m = (cfg.analog_mode >= 0 && (size_t)cfg.analog_mode < nm) ? &all[cfg.analog_mode]
                                                                                         : hdcap_find_mode(input, &sig);
            if (m) {
                logf_(s, HDCAP_LOG_INFO, "analog mode: %s%s, offset %+d/%+d", m->name, cfg.analog_mode >= 0 ? " (manual)" : "",
                      cfg.h_offset, cfg.v_offset);
                applied = m;
                if ((r = hdcap_set_mode(dev, m, cfg.h_offset, cfg.v_offset)) < 0)
                    logf_(s, HDCAP_LOG_WARNING, "set mode failed: %s", hdcap_strerror(r));
                snprintf(detail, sizeof detail, "%s", m->name);
            } else {
                logf_(s, HDCAP_LOG_WARNING, "no standard mode matches this signal; using default sampling");
            }
        }

        hdcap_signal locked_sig = sig;
        hdcap_demux *dx = hdcap_demux_new(on_frame, on_audio, s);
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
        uint64_t next_check = hdcap_now_ns() + SIGNAL_CHECK_NS;
        while (!should_leave(s)) {
            if ((r = hdcap_stream_poll(dev, 100)) < 0) {
                logf_(s, HDCAP_LOG_WARNING, "USB error: %s", hdcap_strerror(r));
                break;
            }
            uint64_t now = hdcap_now_ns();
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

void hdcap_session_set_config(hdcap_session *s, const hdcap_session_config *cfg)
{
    pthread_mutex_lock(&s->lock);
    s->cfg = *cfg;
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
