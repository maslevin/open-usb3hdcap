/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * OBS source for the StarTech USB3HDCAP, built on libhdcap.
 *
 * An hdcap_session (lib/hdcap_session.c) runs the capture loop on its own thread: open the
 * device, select the configured input, wait for a stable signal, set it up, stream, and start
 * over when the device disappears, the signal changes or the settings change. This file turns its
 * callbacks into OBS output: YUY2 frames, and 48 kHz stereo audio resampled to the host clock.
 */
#include <obs-module.h>
#include <util/platform.h>

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
#define S_BRIGHT        "sd_brightness"
#define S_CONTRAST      "sd_contrast"
#define S_SAT           "sd_saturation"
#define S_HUE           "sd_hue"
#define S_SHARP         "sd_sharpness"
#define AUDIO_CHUNK     1024 /* stereo frames per obs_source_output_audio call */

struct hdcap_source {
    obs_source_t  *source;
    hdcap_session *session;

    /* stream state (session worker thread only) */
    hdcap_session_state state;      /* last reported */
    int16_t  audio[AUDIO_CHUNK * 2];
    size_t   audio_fill;
    uint64_t audio_chunk_ts;        /* timestamp of audio[0] */
    hdcap_resampler *resampler;     /* card audio clock -> host clock */
    uint64_t audio_next_log_ns;
};

static const char *hdcap_get_name(void *unused)
{
    UNUSED_PARAMETER(unused);
    return obs_module_text("USB3HDCAP");
}

/* ---------- libhdcap callbacks (worker thread) ---------- */

static void on_frame(const hdcap_frame *f, uint64_t now_ns, void *user)
{
    (void)now_ns; /* stamp with OBS's own clock (same timebase on macOS, but don't depend on it) */
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
    a.timestamp = s->audio_chunk_ts;
    obs_source_output_audio(s->source, &a);
    s->audio_fill = 0;
}

/* The card's audio clock is not the host's (one HDMI source measured -218 ppm). Feeding it to OBS
 * with arrival timestamps made OBS reset the source's audio timing (an audible skip) whenever
 * sample count and timestamps diverged by 70 ms, every ~5 minutes. The resampler converts to the
 * host's 48 kHz with continuous timestamps that track the true capture time. */
static void on_audio(const int16_t *samples, size_t count, uint64_t now_ns, void *user)
{
    struct hdcap_source *s = user;
    (void)now_ns;
    int16_t out[(2048 + 16) * 2];
    uint64_t now = os_gettime_ns();
    while (count) {
        size_t in_n = count > 2048 ? 2048 : count;
        uint64_t ts;
        size_t n = hdcap_resampler_process(s->resampler, samples, in_n, now, out, 2048 + 16, &ts);
        samples += in_n * 2;
        count -= in_n;
        /* a timeline reset (signal gap) must not be glued onto a partly filled chunk */
        if (s->audio_fill && n) {
            int64_t expect = (int64_t)(s->audio_chunk_ts + (uint64_t)s->audio_fill * 1000000000ULL / 48000);
            if (llabs((int64_t)ts - expect) > 1000000)
                flush_audio(s);
        }
        for (size_t i = 0; i < n;) {
            if (!s->audio_fill)
                s->audio_chunk_ts = ts + (uint64_t)i * 1000000000ULL / 48000;
            size_t k = AUDIO_CHUNK - s->audio_fill;
            if (k > n - i)
                k = n - i;
            memcpy(s->audio + s->audio_fill * 2, out + i * 2, k * 4);
            s->audio_fill += k;
            i += k;
            if (s->audio_fill == AUDIO_CHUNK)
                flush_audio(s);
        }
    }
    if (now >= s->audio_next_log_ns) {
        hdcap_resampler_stats st;
        hdcap_resampler_get_stats(s->resampler, &st);
        if (s->audio_next_log_ns)
            blog(LOG_INFO, "[usb3hdcap] audio clock: card %.3f Hz (%+.0f ppm), timeline error %+.2f ms, resets %llu",
                 st.card_rate_hz, (st.card_rate_hz / 48000 - 1) * 1e6, st.error_ms, (unsigned long long)st.resets);
        s->audio_next_log_ns = now + 300000000000ULL;
    }
}

static void on_status(const hdcap_session_status *st, void *user)
{
    struct hdcap_source *s = user;
    if (st->state == s->state)
        return;
    if (s->state == HDCAP_SESSION_STREAMING) {
        flush_audio(s);
        obs_source_output_video(s->source, NULL); /* blank the source until frames return */
    }
    if (st->state == HDCAP_SESSION_STREAMING) {
        hdcap_resampler_reset(s->resampler); /* new stream: new audio timeline */
        s->audio_fill = 0;
        s->audio_next_log_ns = 0;
    }
    if (st->state == HDCAP_SESSION_NO_DEVICE && s->state != HDCAP_SESSION_NO_DEVICE)
        blog(LOG_INFO, "[usb3hdcap] device not available (%s); waiting for it", st->detail);
    s->state = st->state;
}

static void on_log(int level, const char *msg, void *user)
{
    (void)user;
    blog(level == HDCAP_LOG_ERROR ? LOG_ERROR : level == HDCAP_LOG_WARNING ? LOG_WARNING
         : level == HDCAP_LOG_DEBUG ? LOG_DEBUG : LOG_INFO, "[usb3hdcap] %s", msg);
}

/* ---------- OBS source callbacks ---------- */

static hdcap_session_config config_from(obs_data_t *settings)
{
    hdcap_session_config c = {0};
    c.input = (hdcap_input)obs_data_get_int(settings, S_INPUT);
    c.analog_mode = (int)obs_data_get_int(settings, S_MODE);
    c.h_offset = (int)obs_data_get_int(settings, S_HOFF);
    c.v_offset = (int)obs_data_get_int(settings, S_VOFF);
    c.tv_standard = (hdcap_std)obs_data_get_int(settings, S_TVSTD);
    c.picture_set = 1;
    c.picture.brightness = (int)obs_data_get_int(settings, S_BRIGHT);
    c.picture.contrast = (int)obs_data_get_int(settings, S_CONTRAST);
    c.picture.saturation = (int)obs_data_get_int(settings, S_SAT);
    c.picture.hue = (int)obs_data_get_int(settings, S_HUE);
    c.picture.sharpness = (int)obs_data_get_int(settings, S_SHARP);
    return c;
}

static void hdcap_update(void *data, obs_data_t *settings)
{
    struct hdcap_source *s = data;
    hdcap_session_config c = config_from(settings);
    if (s->session)
        hdcap_session_set_config(s->session, &c);   /* restarts the capture loop */
}

static void *hdcap_create(obs_data_t *settings, obs_source_t *source)
{
    struct hdcap_source *s = bzalloc(sizeof(*s));
    s->source = source;
    s->resampler = hdcap_resampler_new();
    s->state = HDCAP_SESSION_STARTING;
    hdcap_session_config c = config_from(settings);
    hdcap_session_callbacks cb = { on_frame, on_audio, on_status, on_log, s };
    s->session = hdcap_session_start(&c, &cb);
    if (!s->session)
        blog(LOG_ERROR, "[usb3hdcap] could not start the capture session");
    return s;
}

static void hdcap_destroy(void *data)
{
    struct hdcap_source *s = data;
    hdcap_session_stop(s->session);   /* joins the worker: no callbacks after this */
    hdcap_resampler_free(s->resampler);
    bfree(s);
}

static void hdcap_defaults(obs_data_t *settings)
{
    obs_data_set_default_int(settings, S_INPUT, HDCAP_INPUT_HDMI);
    obs_data_set_default_int(settings, S_MODE, -1);
    obs_data_set_default_int(settings, S_HOFF, 0);
    obs_data_set_default_int(settings, S_VOFF, 0);
    obs_data_set_default_int(settings, S_TVSTD, HDCAP_STD_AUTO);
    const hdcap_picture pd = HDCAP_PICTURE_DEFAULTS;
    obs_data_set_default_int(settings, S_BRIGHT, pd.brightness);
    obs_data_set_default_int(settings, S_CONTRAST, pd.contrast);
    obs_data_set_default_int(settings, S_SAT, pd.saturation);
    obs_data_set_default_int(settings, S_HUE, pd.hue);
    obs_data_set_default_int(settings, S_SHARP, pd.sharpness);
}

/* Show only the settings that apply to the selected input. */
static bool input_changed(obs_properties_t *props, obs_property_t *prop, obs_data_t *settings)
{
    (void)prop;
    int in = (int)obs_data_get_int(settings, S_INPUT);
    bool sd = in == HDCAP_INPUT_COMPOSITE || in == HDCAP_INPUT_SVIDEO;
    bool analog = in == HDCAP_INPUT_VGA || in == HDCAP_INPUT_COMPONENT;
    const char *sd_keys[] = { S_TVSTD, S_BRIGHT, S_CONTRAST, S_SAT, S_HUE, S_SHARP };
    const char *an_keys[] = { S_MODE, S_HOFF, S_VOFF };
    for (size_t i = 0; i < sizeof sd_keys / sizeof *sd_keys; i++)
        obs_property_set_visible(obs_properties_get(props, sd_keys[i]), sd);
    for (size_t i = 0; i < sizeof an_keys / sizeof *an_keys; i++)
        obs_property_set_visible(obs_properties_get(props, an_keys[i]), analog);
    return true;
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
    /* Composite/S-Video picture (TW9900); applied live without restarting capture */
    obs_properties_add_int_slider(p, S_BRIGHT, obs_module_text("Brightness"), -128, 127, 1);
    obs_properties_add_int_slider(p, S_CONTRAST, obs_module_text("Contrast"), 0, 255, 1);
    obs_properties_add_int_slider(p, S_SAT, obs_module_text("Saturation"), 0, 255, 1);
    obs_properties_add_int_slider(p, S_HUE, obs_module_text("Hue"), -128, 127, 1);
    obs_properties_add_int_slider(p, S_SHARP, obs_module_text("Sharpness"), 0, 15, 1);
    obs_property_set_modified_callback(in, input_changed);
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
