/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * hdcapd XPC protocol, shared by the daemon and its clients (camera extension, HAL plug-in,
 * host app, test client). All messages are XPC dictionaries.
 *
 * Client -> daemon (each gets a reply dictionary with "ok" and the fields noted):
 *   { "cmd": "hello", "client": <string>, "video": <bool>, "audio": <bool> }
 *       reply: "audio_ring" (xpc_shmem, if audio requested), "status" fields
 *       After a hello with video=true the daemon pushes frame messages on the connection.
 *   { "cmd": "get-status" }                     reply: status fields
 *   { "cmd": "set-config", "input", "analog_mode", "h_offset", "v_offset", "tv_standard" (int64) }
 *   { "cmd": "set-deinterlace", "mode" (int64, hdcap_deint_mode) }   persisted; no capture restart
 *
 * Daemon -> client:
 *   { "type": "frame", "surface": <IOSurface xpc>, "width", "height", "interlaced",
 *     "deinterlace" (int64: what the client should apply to interlaced frames),
 *     "seq" (uint64), "time_ns" (uint64, hdcap_now_ns at capture) }
 *   { "type": "status", status fields }        (on changes)
 *
 * Status fields: "state" (int64, hdcap_session_state), "state_name", "input", "width", "height",
 *   "interlaced", "fps" (double), "link_mbps", "detail" (string), "vtotal", "hfreq_hz", "vfreq_hz",
 *   "deinterlace", "hdcp_blocked" (bool: HDCP-protected source, frames are a notice, audio silent).
 *
 * Frames: IOSurfaces from a round-robin pool owned by the daemon, pixel format 'yuvs'
 * (kCVPixelFormatType_422YpCbCr8_yuvs: Y0 Cb Y1 Cr). A client must finish with (or copy) a
 * surface before HDCAPD_SURFACE_POOL newer frames arrive.
 */
#ifndef HDCAPD_PROTOCOL_H
#define HDCAPD_PROTOCOL_H

#include <stdatomic.h>
#include <stdint.h>

#define HDCAPD_SERVICE_NAME  "org.open-usb3hdcap.hdcapd"
#define HDCAPD_SURFACE_POOL  8

/* Audio ring buffer in shared memory (mapped read-only by clients). */
#define HDCAPD_RING_MAGIC    0x48444341u   /* 'HDCA' */
#define HDCAPD_RING_VERSION  1
#define HDCAPD_RING_FRAMES   (48000 * 2)   /* 2 s of 48 kHz stereo */

typedef struct {
    uint32_t magic, version;
    uint32_t sample_rate, channels;        /* 48000, 2 */
    uint32_t capacity_frames;              /* HDCAPD_RING_FRAMES */
    uint32_t bytes_per_sample;             /* 2 (int16 LE, interleaved L R) */
    _Atomic uint64_t write_frames;         /* total frames ever written (monotonic) */
    _Atomic uint64_t write_time_ns;        /* hdcap_now_ns() when write_frames was last advanced */
    _Atomic uint32_t generation;           /* bumps when the stream restarts (discontinuity) */
    uint32_t reserved[9];
    int16_t  data[];                       /* capacity_frames * channels samples */
} hdcapd_audio_ring;

#define HDCAPD_RING_BYTES (sizeof(hdcapd_audio_ring) + (size_t)HDCAPD_RING_FRAMES * 2 * sizeof(int16_t))

#endif
