/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * OBS source for the StarTech USB3HDCAP, built on libhdcap.
 *
 * A worker thread opens the device, selects the configured input, waits for a stable signal, streams,
 * and outputs YUY2 frames and 48 kHz stereo audio to OBS. If the device disappears, the signal is lost
 * or the settings change, the thread starts over.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdatomic.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hdcap.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("usb3hdcap", "en-US")

#define S_INPUT         "input"
#define S_MODE          "analog_mode"   /* -1 = auto, else index into hdcap_modes() */
#define S_HOFF          "h_offset"
#define S_VOFF          "v_offset"
#define S_TVSTD         "tv_standard"
#define SIGNAL_CHECK_NS (1000000000ULL)
#define NO_FRAME_TIMEOUT_NS (3ULL * 1000000000ULL)
#define AUDIO_CHUNK     1024 /* stereo frames per obs_source_output_audio call */

struct hdcap_source {
    obs_source_t *source;
    pthread_t     thread;
    bool          thread_running;
    atomic_bool   stop;
    os_event_t   *restart;          /* signalled when settings change */

    pthread_mutex_t settings_lock;
    int             input, mode, hoff, voff, tvstd;

    /* stream state (worker thread only) */
    uint64_t last_frame_ns;
    int16_t  audio[AUDIO_CHUNK * 2];
    size_t   audio_fill;
};

static const char *hdcap_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("USB3HDCAP");
}

/* ---------- libhdcap callbacks (worker thread) ---------- */

static void on_frame(const hdcap_frame *f, void *user)
{
    struct hdcap_source *s = user;
    struct obs_source_frame frame = {0};
    frame.format = VIDEO_FORMAT_YUY2;
    frame.width = (uint32_t)f->width;
    frame.height = (uint32_t)f->height;
    frame.data[0] = (uint8_t *)f->yuyv;
    frame.linesize[0] = (uint32_t)f->width * 2;
    frame.timestamp = os_gettime_ns();
    frame.full_range = false;
    video_format_get_parameters(VIDEO_CS_709, VIDEO_RANGE_PARTIAL, frame.color_matrix,
                                frame.color_range_min, frame.color_range_max);
    if (f->height <= 576) /* SD sources use BT.601 */
        video_format_get_parameters(VIDEO_CS_601, VIDEO_RANGE_PARTIAL, frame.color_matrix,
                                    frame.color_range_min, frame.color_range_max);
    s->last_frame_ns = frame.timestamp;
    obs_source_output_video(s->source, &frame);
}

static void flush_audio(struct hdcap_source *s)
{
    if (!s->audio_fill)
        return;
    struct obs_source_audio a = {0};
    a.data[0] = (const uint8_t *)s->audio;
    a.frames = (uint32_t)s->audio_fill;
    a.speakers = SPEAKERS_STEREO;
    a.format = AUDIO_FORMAT_16BIT;
    a.samples_per_sec = 48000;
    /* Timestamp of the first sample in the chunk, estimated from arrival time. */
    a.timestamp = os_gettime_ns() - (uint64_t)s->audio_fill * 1000000000ULL / 48000;
    obs_source_output_audio(s->source, &a);
    s->audio_fill = 0;
}

static void on_audio(const int16_t *samples, size_t count, void *user)
{
    struct hdcap_source *s = user;
    while (count) {
        size_t n = AUDIO_CHUNK - s->audio_fill;
        if (n > count)
            n = count;
        memcpy(s->audio + s->audio_fill * 2, samples, n * 4);
        s->audio_fill += n;
        samples += n * 2;
        count -= n;
        if (s->audio_fill == AUDIO_CHUNK)
            flush_audio(s);
    }
}

/* ---------- worker thread ---------- */

static bool wait_or_stop(struct hdcap_source *s, unsigned ms)
{
    /* Returns true if the thread should exit. A settings change also ends the wait. */
    os_event_timedwait(s->restart, ms);
    return atomic_load(&s->stop);
}

static void *worker(void *data)
{
    struct hdcap_source *s = data;
    os_set_thread_name("usb3hdcap");

    while (!atomic_load(&s->stop)) {
        os_event_reset(s->restart);
        pthread_mutex_lock(&s->settings_lock);
        hdcap_input input = (hdcap_input)s->input;
        int mode_sel = s->mode, hoff = s->hoff, voff = s->voff, tvstd = s->tvstd;
        pthread_mutex_unlock(&s->settings_lock);

        hdcap *dev = NULL;
        int r = hdcap_open(&dev);
        if (r < 0) {
            blog(LOG_INFO, "[usb3hdcap] open: %s; retrying", hdcap_strerror(r));
            if (wait_or_stop(s, 2000))
                break;
            continue;
        }
        if ((r = hdcap_init(dev)) < 0 || (r = hdcap_select_input(dev, input)) < 0) {
            blog(LOG_WARNING, "[usb3hdcap] select %s failed: %s", hdcap_input_name(input), hdcap_strerror(r));
            hdcap_close(dev);
            if (wait_or_stop(s, 2000))
                break;
            continue;
        }
        int mbps = hdcap_link_speed_mbps(dev);
        if (mbps < 5000)
            blog(LOG_WARNING, "[usb3hdcap] device is connected at %d Mbit/s, not USB 3 SuperSpeed: only "
                 "Composite/S-Video fit through USB 2.0. Use a USB 3 port, hub and cable.", mbps);
        blog(LOG_INFO, "[usb3hdcap] %s selected; waiting for signal", hdcap_input_name(input));
        hdcap_signal sig;
        int locked = 0;
        while (!atomic_load(&s->stop) && os_event_try(s->restart) == EAGAIN) {
            if ((locked = hdcap_wait_signal(dev, input, 1000, &sig)) != 0)
                break;
        }
        if (locked <= 0) {
            if (locked < 0)
                blog(LOG_WARNING, "[usb3hdcap] signal read failed: %s", hdcap_strerror(locked));
            hdcap_close(dev);
            if (wait_or_stop(s, 500))
                break;
            continue;
        }
        blog(LOG_INFO, "[usb3hdcap] signal: %s%s%d lines, %.3f kHz, %.2f Hz%s", sig.standard ? sig.standard : "",
             sig.standard ? ", " : "", sig.vtotal, sig.hfreq_hz / 1000, sig.vfreq_hz, sig.interlaced ? ", interlaced" : "");
        if (input == HDCAP_INPUT_COMPOSITE || input == HDCAP_INPUT_SVIDEO) {
            int st = hdcap_setup_sd(dev, (hdcap_std)tvstd, 2000);
            if (st >= 0)
                blog(LOG_INFO, "[usb3hdcap] TV standard: %s%s", hdcap_std_name((hdcap_std)st),
                     tvstd == HDCAP_STD_AUTO ? " (detected)" : " (forced)");
        }
        if (input == HDCAP_INPUT_HDMI || input == HDCAP_INPUT_DVI) {
            hdcap_digital_setup ds;
            if ((r = hdcap_setup_digital(dev, 3000, &ds)) < 0)
                blog(LOG_WARNING, "[usb3hdcap] digital setup failed: %s", hdcap_strerror(r));
            else
                blog(LOG_INFO, "[usb3hdcap] digital: AVI %s, VIC %d, %s, %s audio", ds.avi.valid ? "present" : "absent",
                     ds.avi.vic, ds.ycbcr_passthru ? "YCbCr" : ds.full_range ? "RGB full range" : "RGB limited range",
                     ds.embedded_audio ? "embedded" : "line-in");
        }
        const hdcap_mode *applied_mode = NULL;
        if (input == HDCAP_INPUT_VGA || input == HDCAP_INPUT_COMPONENT) {
            size_t nm;
            const hdcap_mode *all = hdcap_modes(&nm);
            const hdcap_mode *m = (mode_sel >= 0 && (size_t)mode_sel < nm) ? &all[mode_sel] : hdcap_find_mode(input, &sig);
            if (m) {
                blog(LOG_INFO, "[usb3hdcap] analog mode: %s%s, offset %+d/%+d", m->name, mode_sel >= 0 ? " (manual)" : "",
                     hoff, voff);
                applied_mode = m;
                if ((r = hdcap_set_mode(dev, m, hoff, voff)) < 0)
                    blog(LOG_WARNING, "[usb3hdcap] set mode failed: %s", hdcap_strerror(r));
            } else {
                blog(LOG_WARNING, "[usb3hdcap] no standard mode matches this signal; using default sampling");
            }
        }
        hdcap_signal locked_sig = sig;
        uint64_t next_check = os_gettime_ns() + SIGNAL_CHECK_NS;

        hdcap_demux *dx = hdcap_demux_new(on_frame, on_audio, s);
        double rate = hdcap_estimate_rate(input, &sig, applied_mode);
        blog(LOG_INFO, "[usb3hdcap] expected stream rate %.1f MB/s", rate / 1e6);
        if (!dx || (r = hdcap_stream_start_rate(dev, dx, rate)) < 0) {
            blog(LOG_WARNING, "[usb3hdcap] stream start failed: %s", hdcap_strerror(dx ? r : HDCAP_ERR_NOMEM));
            hdcap_demux_free(dx);
            hdcap_close(dev);
            if (wait_or_stop(s, 2000))
                break;
            continue;
        }
        blog(LOG_INFO, "[usb3hdcap] streaming");
        s->last_frame_ns = os_gettime_ns();
        s->audio_fill = 0;
        while (!atomic_load(&s->stop) && os_event_try(s->restart) == EAGAIN) {
            r = hdcap_stream_poll(dev, 100);
            if (r < 0) {
                blog(LOG_WARNING, "[usb3hdcap] USB error: %s", hdcap_strerror(r));
                break;
            }
            if (os_gettime_ns() >= next_check) {
                /* Re-check the signal: a lost lock or a timing change restarts detection. */
                next_check = os_gettime_ns() + SIGNAL_CHECK_NS;
                hdcap_signal now;
                if (hdcap_get_signal(dev, input, &now) == 0 &&
                    (!now.locked || abs(now.vtotal - locked_sig.vtotal) > 3 ||
                     fabs(now.hfreq_hz - locked_sig.hfreq_hz) > locked_sig.hfreq_hz * 0.01)) {
                    blog(LOG_INFO, "[usb3hdcap] signal changed (%d lines, %.3f kHz); restarting", now.vtotal,
                         now.hfreq_hz / 1000);
                    obs_source_output_video(s->source, NULL);
                    break;
                }
            }
            if (os_gettime_ns() - s->last_frame_ns > NO_FRAME_TIMEOUT_NS) {
                blog(LOG_INFO, "[usb3hdcap] no frames for 3 s; restarting");
                obs_source_output_video(s->source, NULL); /* blank the source */
                break;
            }
        }
        hdcap_stream_stop(dev);
        hdcap_demux_free(dx);
        hdcap_close(dev);
        if (wait_or_stop(s, 500))
            break;
    }
    return NULL;
}

/* ---------- OBS source callbacks ---------- */

static void hdcap_update(void *data, obs_data_t *settings)
{
    struct hdcap_source *s = data;
    pthread_mutex_lock(&s->settings_lock);
    s->input = (int)obs_data_get_int(settings, S_INPUT);
    s->mode = (int)obs_data_get_int(settings, S_MODE);
    s->hoff = (int)obs_data_get_int(settings, S_HOFF);
    s->voff = (int)obs_data_get_int(settings, S_VOFF);
    s->tvstd = (int)obs_data_get_int(settings, S_TVSTD);
    pthread_mutex_unlock(&s->settings_lock);
    os_event_signal(s->restart);
}

static void *hdcap_create(obs_data_t *settings, obs_source_t *source)
{
    struct hdcap_source *s = bzalloc(sizeof(*s));
    s->source = source;
    pthread_mutex_init(&s->settings_lock, NULL);
    os_event_init(&s->restart, OS_EVENT_TYPE_MANUAL);
    atomic_init(&s->stop, false);
    hdcap_update(s, settings);
    if (pthread_create(&s->thread, NULL, worker, s) == 0)
        s->thread_running = true;
    return s;
}

static void hdcap_destroy(void *data)
{
    struct hdcap_source *s = data;
    atomic_store(&s->stop, true);
    os_event_signal(s->restart);
    if (s->thread_running)
        pthread_join(s->thread, NULL);
    os_event_destroy(s->restart);
    pthread_mutex_destroy(&s->settings_lock);
    bfree(s);
}

static void hdcap_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, S_INPUT, HDCAP_INPUT_HDMI);
    obs_data_set_default_int(settings, S_MODE, -1);
    obs_data_set_default_int(settings, S_HOFF, 0);
    obs_data_set_default_int(settings, S_VOFF, 0);
    obs_data_set_default_int(settings, S_TVSTD, HDCAP_STD_AUTO);
}

static obs_properties_t *hdcap_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *p = obs_properties_create();
    obs_property_t *in = obs_properties_add_list(p, S_INPUT, obs_module_text("Input"), OBS_COMBO_TYPE_LIST,
                                                 OBS_COMBO_FORMAT_INT);
    for (int i = 0; i < HDCAP_INPUT_COUNT; i++)
        obs_property_list_add_int(in, hdcap_input_name((hdcap_input)i), i);
    obs_property_t *md = obs_properties_add_list(p, S_MODE, obs_module_text("AnalogMode"), OBS_COMBO_TYPE_LIST,
                                                 OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(md, obs_module_text("Auto"), -1);
    size_t nm;
    const hdcap_mode *all = hdcap_modes(&nm);
    for (size_t i = 0; i < nm; i++)
        obs_property_list_add_int(md, all[i].name, (long long)i);
    obs_property_set_long_description(md, obs_module_text("AnalogModeHelp"));
    obs_property_t *sd = obs_properties_add_list(p, S_TVSTD, obs_module_text("TVStandard"), OBS_COMBO_TYPE_LIST,
                                                 OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(sd, obs_module_text("Auto"), HDCAP_STD_AUTO);
    for (int i = 0; i < HDCAP_STD_AUTO; i++)
        obs_property_list_add_int(sd, hdcap_std_name((hdcap_std)i), i);
    obs_properties_add_int_slider(p, S_HOFF, obs_module_text("HOffset"), -200, 200, 1);
    obs_properties_add_int_slider(p, S_VOFF, obs_module_text("VOffset"), -60, 60, 1);
    return p;
}

static struct obs_source_info hdcap_source_info = {
    .id = "usb3hdcap_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
    .get_name = hdcap_get_name,
    .create = hdcap_create,
    .destroy = hdcap_destroy,
    .update = hdcap_update,
    .get_defaults = hdcap_defaults,
    .get_properties = hdcap_properties,
    .icon_type = OBS_ICON_TYPE_CAMERA,
};

bool obs_module_load(void)
{
    obs_register_source(&hdcap_source_info);
    blog(LOG_INFO, "[usb3hdcap] plugin loaded");
    return true;
}
