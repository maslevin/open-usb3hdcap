/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * hdcapd: owns the USB3HDCAP and serves video (IOSurfaces) and audio (shared ring buffer) to the
 * camera extension, the HAL plug-in and other clients over XPC. See docs/MACOS_DRIVERS.md and
 * macos/common/hdcapd_protocol.h.
 *
 * The device is opened only while at least one client is connected and released a few seconds
 * after the last one disconnects, so other software (e.g. the OBS plugin) can use the card when
 * the system drivers are idle.
 *
 *   hdcapd [--service NAME] [--config FILE]
 * Normally started on demand by launchd (MachServices). See macos/scripts/dev-agent.sh.
 */
#include "hdcap.h"
#include "../common/hdcapd_protocol.h"

#include <CoreFoundation/CoreFoundation.h>
#include <xpc/xpc.h> /* before IOSurface: enables IOSurfaceCreateXPCObject */
#include <IOSurface/IOSurfaceAPI.h>
#include <dispatch/dispatch.h>

#include <errno.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define IDLE_RELEASE_SEC 3

typedef struct client {
    xpc_connection_t conn;
    char  name[64];
    int   video, audio;
    struct client *next;
} client;

static const char *g_service = HDCAPD_SERVICE_NAME;
static char g_config_path[1024];
static dispatch_queue_t g_q;              /* serialises client list, session lifetime, config */
static client *g_clients;
static hdcap_session *g_session;
static hdcap_session_config g_cfg = { HDCAP_INPUT_HDMI, -1, 0, 0, HDCAP_STD_AUTO, 0, { 0, 0, 0, 0, 0 } };
/* Deinterlacing for camera clients (hdcap_deint_mode). Not a session setting: it is stamped on every
 * frame message and applied by the client, so changing it never restarts capture. */
static _Atomic int g_deint = HDCAP_DEINT_ADAPTIVE;
static uint64_t g_idle_token;             /* cancels pending idle releases */

/* Audio ring (shared memory) */
static hdcapd_audio_ring *g_ring;
static xpc_object_t g_ring_shmem;

/* Surface pool (session thread only, except for teardown) */
static IOSurfaceRef g_pool[HDCAPD_SURFACE_POOL];
static int g_pool_w, g_pool_h, g_pool_next;
static uint64_t g_seq;

static void logmsg(const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "%s hdcapd: ", ts);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ---------- configuration file: key=value lines ---------- */

static void load_config(void)
{
    FILE *f = fopen(g_config_path, "r");
    if (!f)
        return;
    char k[64];
    int v;
    while (fscanf(f, " %63[^=]=%d", k, &v) == 2) {
        if (!strcmp(k, "input") && v >= 0 && v < HDCAP_INPUT_COUNT) g_cfg.input = (hdcap_input)v;
        else if (!strcmp(k, "analog_mode")) g_cfg.analog_mode = v;
        else if (!strcmp(k, "h_offset")) g_cfg.h_offset = v;
        else if (!strcmp(k, "v_offset")) g_cfg.v_offset = v;
        else if (!strcmp(k, "tv_standard") && v >= 0 && v <= HDCAP_STD_AUTO) g_cfg.tv_standard = (hdcap_std)v;
        else if (!strcmp(k, "deinterlace") && v >= HDCAP_DEINT_OFF && v <= HDCAP_DEINT_ADAPTIVE) g_deint = v;
    }
    fclose(f);
}

static void save_config(void)
{
    FILE *f = fopen(g_config_path, "w");
    if (!f) {
        logmsg("cannot write %s: %s", g_config_path, strerror(errno));
        return;
    }
    fprintf(f, "input=%d\nanalog_mode=%d\nh_offset=%d\nv_offset=%d\ntv_standard=%d\ndeinterlace=%d\n", g_cfg.input,
            g_cfg.analog_mode, g_cfg.h_offset, g_cfg.v_offset, g_cfg.tv_standard, (int)g_deint);
    fclose(f);
}

/* ---------- status ---------- */

static void put_status(xpc_object_t d, const hdcap_session_status *st)
{
    xpc_dictionary_set_int64(d, "state", st->state);
    xpc_dictionary_set_string(d, "state_name", hdcap_session_state_name(st->state));
    xpc_dictionary_set_int64(d, "input", st->input);
    xpc_dictionary_set_string(d, "input_name", hdcap_input_name(st->input));
    xpc_dictionary_set_int64(d, "width", st->width);
    xpc_dictionary_set_int64(d, "height", st->height);
    xpc_dictionary_set_bool(d, "interlaced", st->interlaced);
    xpc_dictionary_set_double(d, "fps", st->fps);
    xpc_dictionary_set_int64(d, "link_mbps", st->link_mbps);
    xpc_dictionary_set_string(d, "detail", st->detail);
    xpc_dictionary_set_int64(d, "vtotal", st->signal.vtotal);
    xpc_dictionary_set_double(d, "hfreq_hz", st->signal.hfreq_hz);
    xpc_dictionary_set_double(d, "vfreq_hz", st->signal.vfreq_hz);
    xpc_dictionary_set_int64(d, "deinterlace", g_deint);
    xpc_dictionary_set_bool(d, "hdcp_blocked", st->hdcp_blocked);
}

static void current_status(hdcap_session_status *st)
{
    memset(st, 0, sizeof *st);
    if (g_session) {
        hdcap_session_get_status(g_session, st);
    } else {
        st->state = HDCAP_SESSION_STOPPED;
        st->input = g_cfg.input;
        snprintf(st->detail, sizeof st->detail, "idle (no clients)");
    }
}

/* ---------- session callbacks (session worker thread) ---------- */

static IOSurfaceRef pool_surface(int w, int h)
{
    if (w != g_pool_w || h != g_pool_h) {
        for (int i = 0; i < HDCAPD_SURFACE_POOL; i++) {
            if (g_pool[i])
                CFRelease(g_pool[i]);
            g_pool[i] = NULL;
        }
        g_pool_w = w;
        g_pool_h = h;
    }
    int i = g_pool_next;
    g_pool_next = (g_pool_next + 1) % HDCAPD_SURFACE_POOL;
    if (!g_pool[i]) {
        /* Packed 4:2:2: one element = 2 pixels = 4 bytes (Y0 Cb Y1 Cr). Rows aligned to 64 bytes. */
        int32_t pw = w, ph = h, bpe = 4, bw = 2, bh = 1, bpr = (w * 2 + 63) & ~63;
        uint32_t fmt = 'yuvs';
        CFNumberRef nw = CFNumberCreate(NULL, kCFNumberSInt32Type, &pw);
        CFNumberRef nh = CFNumberCreate(NULL, kCFNumberSInt32Type, &ph);
        CFNumberRef nb = CFNumberCreate(NULL, kCFNumberSInt32Type, &bpe);
        CFNumberRef nbw = CFNumberCreate(NULL, kCFNumberSInt32Type, &bw);
        CFNumberRef nbh = CFNumberCreate(NULL, kCFNumberSInt32Type, &bh);
        CFNumberRef nf = CFNumberCreate(NULL, kCFNumberSInt32Type, &fmt);
        CFNumberRef nr = CFNumberCreate(NULL, kCFNumberSInt32Type, &bpr);
        const void *keys[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement,
                               kIOSurfaceElementWidth, kIOSurfaceElementHeight, kIOSurfacePixelFormat,
                               kIOSurfaceBytesPerRow };
        const void *vals[] = { nw, nh, nb, nbw, nbh, nf, nr };
        CFDictionaryRef props = CFDictionaryCreate(NULL, keys, vals, 7, &kCFTypeDictionaryKeyCallBacks,
                                                   &kCFTypeDictionaryValueCallBacks);
        CFRelease(nr);
        g_pool[i] = IOSurfaceCreate(props);
        CFRelease(props);
        CFRelease(nw); CFRelease(nh); CFRelease(nb); CFRelease(nbw); CFRelease(nbh); CFRelease(nf);
    }
    return g_pool[i];
}

static void on_frame(const hdcap_frame *f, uint64_t now_ns, void *user)
{
    (void)user;
    IOSurfaceRef surf = pool_surface(f->width, f->height);
    if (!surf)
        return;
    IOSurfaceLock(surf, 0, NULL);
    uint8_t *dst = IOSurfaceGetBaseAddress(surf);
    size_t dstride = IOSurfaceGetBytesPerRow(surf), sstride = (size_t)f->width * 2;
    if (!dst || dstride < sstride || IOSurfaceGetHeight(surf) < (size_t)f->height) {
        IOSurfaceUnlock(surf, 0, NULL);
        return;
    }
    for (int y = 0; y < f->height; y++)
        memcpy(dst + y * dstride, f->yuyv + y * sstride, sstride);
    IOSurfaceUnlock(surf, 0, NULL);

    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "type", "frame");
    xpc_object_t xs = IOSurfaceCreateXPCObject(surf);
    xpc_dictionary_set_value(msg, "surface", xs);
    xpc_release(xs);
    xpc_dictionary_set_int64(msg, "width", f->width);
    xpc_dictionary_set_int64(msg, "height", f->height);
    xpc_dictionary_set_bool(msg, "interlaced", f->interlaced);
    xpc_dictionary_set_int64(msg, "deinterlace", g_deint);
    xpc_dictionary_set_uint64(msg, "seq", g_seq++);
    xpc_dictionary_set_uint64(msg, "time_ns", now_ns);
    dispatch_async(g_q, ^{
        for (client *c = g_clients; c; c = c->next)
            if (c->video)
                xpc_connection_send_message(c->conn, msg);
        xpc_release(msg);
    });
}

static void on_audio(const int16_t *samples, size_t count, uint64_t now_ns, void *user)
{
    (void)user;
    if (!g_ring)
        return;
    uint64_t w = atomic_load_explicit(&g_ring->write_frames, memory_order_relaxed);
    for (size_t i = 0; i < count; i++) {
        size_t slot = (size_t)((w + i) % HDCAPD_RING_FRAMES);
        g_ring->data[slot * 2] = samples[i * 2];
        g_ring->data[slot * 2 + 1] = samples[i * 2 + 1];
    }
    atomic_store_explicit(&g_ring->write_time_ns, now_ns, memory_order_relaxed);
    atomic_store_explicit(&g_ring->write_frames, w + count, memory_order_release);
}

static void on_status(const hdcap_session_status *st, void *user)
{
    (void)user;
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "type", "status");
    put_status(msg, st);
    dispatch_async(g_q, ^{
        for (client *c = g_clients; c; c = c->next)
            xpc_connection_send_message(c->conn, msg);
        xpc_release(msg);
    });
}

static void on_log(int level, const char *message, void *user)
{
    (void)user;
    logmsg("%s%s", level >= HDCAP_LOG_WARNING ? "warning: " : "", message);
}

/* ---------- session lifetime (on g_q) ---------- */

static void ensure_session(void)
{
    g_idle_token++;
    if (g_session)
        return;
    atomic_fetch_add(&g_ring->generation, 1);
    hdcap_session_callbacks cb = { on_frame, on_audio, on_status, on_log, NULL };
    g_session = hdcap_session_start(&g_cfg, &cb);
    logmsg("session started (%s)", hdcap_input_name(g_cfg.input));
}

/* Only clients that asked for video or audio keep the device claimed. Status-only clients (the
 * USB3HDCAP app's control panel, hdcapd-client status) must not, or the card stays busy and other
 * programs (OBS, hdcap-capture) can't open it while the app is open. */
static int streaming_clients(void)
{
    for (client *c = g_clients; c; c = c->next)
        if (c->video || c->audio)
            return 1;
    return 0;
}

static void schedule_idle_release(void)
{
    uint64_t token = ++g_idle_token;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, IDLE_RELEASE_SEC * NSEC_PER_SEC), g_q, ^{
        if (token != g_idle_token || streaming_clients() || !g_session)
            return;
        hdcap_session *s = g_session;
        g_session = NULL;
        hdcap_session_stop(s);     /* joins the worker; callbacks only dispatch_async onto g_q */
        logmsg("no clients: device released");
    });
}

/* ---------- XPC ---------- */

static void handle_message(client *c, xpc_object_t msg)
{
    const char *cmd = xpc_dictionary_get_string(msg, "cmd");
    xpc_object_t reply = xpc_dictionary_create_reply(msg);
    if (!cmd || !reply) {
        if (reply)
            xpc_release(reply);
        return;
    }
    int ok = 1;
    if (!strcmp(cmd, "hello")) {
        const char *name = xpc_dictionary_get_string(msg, "client");
        snprintf(c->name, sizeof c->name, "%s", name ? name : "?");
        c->video = xpc_dictionary_get_bool(msg, "video");
        c->audio = xpc_dictionary_get_bool(msg, "audio");
        if (c->audio)
            xpc_dictionary_set_value(reply, "audio_ring", g_ring_shmem);
        logmsg("client '%s' connected (video %d, audio %d)", c->name, c->video, c->audio);
        ensure_session();
    } else if (!strcmp(cmd, "set-config")) {
        hdcap_session_config n = g_cfg;
        int64_t v = xpc_dictionary_get_int64(msg, "input");
        if (v >= 0 && v < HDCAP_INPUT_COUNT)
            n.input = (hdcap_input)v;
        n.analog_mode = (int)xpc_dictionary_get_int64(msg, "analog_mode");
        n.h_offset = (int)xpc_dictionary_get_int64(msg, "h_offset");
        n.v_offset = (int)xpc_dictionary_get_int64(msg, "v_offset");
        v = xpc_dictionary_get_int64(msg, "tv_standard");
        n.tv_standard = (v >= 0 && v <= HDCAP_STD_AUTO) ? (hdcap_std)v : HDCAP_STD_AUTO;
        g_cfg = n;
        save_config();
        if (g_session)
            hdcap_session_set_config(g_session, &g_cfg);
        logmsg("config: %s, analog mode %d, offset %+d/%+d, standard %s", hdcap_input_name(g_cfg.input), g_cfg.analog_mode,
               g_cfg.h_offset, g_cfg.v_offset, hdcap_std_name(g_cfg.tv_standard));
    } else if (!strcmp(cmd, "set-deinterlace")) {
        int64_t v = xpc_dictionary_get_int64(msg, "mode");
        if (v >= HDCAP_DEINT_OFF && v <= HDCAP_DEINT_ADAPTIVE) {
            g_deint = (int)v;
            save_config();
            logmsg("deinterlace: %s", v == HDCAP_DEINT_OFF ? "off" : v == HDCAP_DEINT_BOB ? "bob" : "adaptive");
        } else {
            ok = 0;
        }
    } else if (strcmp(cmd, "get-status") != 0) {
        ok = 0;
    }
    hdcap_session_status st;
    current_status(&st);
    put_status(reply, &st);
    xpc_dictionary_set_bool(reply, "ok", ok);
    xpc_connection_send_message(c->conn, reply);
    xpc_release(reply);
}

static void remove_client(client *c)
{
    for (client **p = &g_clients; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
    logmsg("client '%s' disconnected", c->name);
    xpc_release(c->conn);
    free(c);
    if (!streaming_clients())
        schedule_idle_release();
}

static void new_connection(xpc_connection_t conn)
{
    client *c = calloc(1, sizeof *c);
    c->conn = conn;
    xpc_retain(conn);
    snprintf(c->name, sizeof c->name, "pid %d", xpc_connection_get_pid(conn));
    c->next = g_clients;
    g_clients = c;
    xpc_connection_set_target_queue(conn, g_q);
    xpc_connection_set_event_handler(conn, ^(xpc_object_t ev) {
        if (xpc_get_type(ev) == XPC_TYPE_DICTIONARY)
            handle_message(c, ev);
        else if (ev == XPC_ERROR_CONNECTION_INVALID)
            remove_client(c);
    });
    xpc_connection_resume(conn);
}

static void make_ring(void)
{
    size_t len = (HDCAPD_RING_BYTES + 0x3FFF) & ~(size_t)0x3FFF;
    void *mem = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED, -1, 0);
    if (mem == MAP_FAILED) {
        logmsg("mmap failed: %s", strerror(errno));
        exit(1);
    }
    g_ring = mem;
    g_ring->magic = HDCAPD_RING_MAGIC;
    g_ring->version = HDCAPD_RING_VERSION;
    g_ring->sample_rate = 48000;
    g_ring->channels = 2;
    g_ring->capacity_frames = HDCAPD_RING_FRAMES;
    g_ring->bytes_per_sample = 2;
    g_ring_shmem = xpc_shmem_create(mem, len);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = { { "service", required_argument, 0, 's' },
                                          { "config", required_argument, 0, 'c' }, { 0, 0, 0, 0 } };
    const char *home = getenv("HOME");
    snprintf(g_config_path, sizeof g_config_path, "%s/.hdcapd.conf", geteuid() == 0 || !home ? "/Library/Preferences" : home);
    int o;
    while ((o = getopt_long(argc, argv, "s:c:", opts, NULL)) != -1) {
        if (o == 's') g_service = optarg;
        else if (o == 'c') snprintf(g_config_path, sizeof g_config_path, "%s", optarg);
        else {
            fprintf(stderr, "usage: hdcapd [--service NAME] [--config FILE]\n");
            return 2;
        }
    }
    load_config();
    make_ring();
    g_q = dispatch_queue_create("hdcapd", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
    xpc_connection_t listener = xpc_connection_create_mach_service(g_service, g_q, XPC_CONNECTION_MACH_SERVICE_LISTENER);
    xpc_connection_set_event_handler(listener, ^(xpc_object_t ev) {
        if (xpc_get_type(ev) == XPC_TYPE_CONNECTION)
            new_connection((xpc_connection_t)ev);
    });
    xpc_connection_resume(listener);
    logmsg("listening on %s (config %s, input %s)", g_service, g_config_path, hdcap_input_name(g_cfg.input));
    dispatch_main();
}
