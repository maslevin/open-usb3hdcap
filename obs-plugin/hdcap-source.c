/*
 * OBS source for the StarTech USB3HDCAP, built on libhdcap.
 *
 * A worker thread opens the device, runs the startup sequence (for now a recorded HDMI sequence,
 * see sequences/README.md), streams, and outputs YUY2 frames and 48 kHz stereo audio to OBS.
 * If the device disappears or stops delivering frames, the thread reconnects.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdatomic.h>
#include <string.h>

#include "hdcap.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("usb3hdcap", "en-US")

#define S_SEQUENCE      "sequence"
#define S_MAX_DELAY     "max_delay_ms"
#define NO_FRAME_TIMEOUT_NS (3ULL * 1000000000ULL)
#define AUDIO_CHUNK     1024 /* stereo frames per obs_source_output_audio call */

struct hdcap_source {
    obs_source_t *source;
    pthread_t     thread;
    bool          thread_running;
    atomic_bool   stop;
    os_event_t   *restart;          /* signalled when settings change */

    pthread_mutex_t settings_lock;
    char           *sequence;
    int             max_delay_ms;

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
        char *seq = bstrdup(s->sequence);
        int max_delay = s->max_delay_ms;
        pthread_mutex_unlock(&s->settings_lock);

        hdcap *dev = NULL;
        int r = hdcap_open(&dev);
        if (r < 0) {
            blog(LOG_INFO, "[usb3hdcap] open: %s; retrying", hdcap_strerror(r));
            bfree(seq);
            if (wait_or_stop(s, 2000))
                break;
            continue;
        }
        blog(LOG_INFO, "[usb3hdcap] device opened; running %s", seq ? seq : "(none)");
        if (seq && *seq && (r = hdcap_run_sequence_ex(dev, seq, 0, max_delay)) < 0) {
            blog(LOG_WARNING, "[usb3hdcap] sequence failed: %s", hdcap_strerror(r));
            hdcap_close(dev);
            bfree(seq);
            if (wait_or_stop(s, 2000))
                break;
            continue;
        }
        bfree(seq);

        hdcap_demux *dx = hdcap_demux_new(on_frame, on_audio, s);
        if (!dx || (r = hdcap_stream_start(dev, dx)) < 0) {
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
    bfree(s->sequence);
    s->sequence = bstrdup(obs_data_get_string(settings, S_SEQUENCE));
    s->max_delay_ms = (int)obs_data_get_int(settings, S_MAX_DELAY);
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
    bfree(s->sequence);
    bfree(s);
}

static void hdcap_defaults(obs_data_t *settings)
{
    char *seq = obs_module_file("sequences/open_hdmi.seq");
    obs_data_set_default_string(settings, S_SEQUENCE, seq ? seq : "");
    bfree(seq);
    obs_data_set_default_int(settings, S_MAX_DELAY, 10);
}

static obs_properties_t *hdcap_properties(void *data)
{
    UNUSED_PARAMETER(data);
    obs_properties_t *p = obs_properties_create();
    obs_properties_add_path(p, S_SEQUENCE, obs_module_text("StartupSequence"), OBS_PATH_FILE,
                            "Sequence files (*.seq)", NULL);
    obs_properties_add_int(p, S_MAX_DELAY, obs_module_text("MaxDelay"), 0, 2000, 10);
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
