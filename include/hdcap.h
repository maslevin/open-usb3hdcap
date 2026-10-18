/*
 * open-usb3hdcap: user-space driver core for the StarTech USB3HDCAP (YUAN UB-530, USB 1164:F533).
 *
 * Written from the observed protocol in docs/PROTOCOL.md (clean-room: no vendor code).
 */
#ifndef HDCAP_H
#define HDCAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HDCAP_VID 0x1164
#define HDCAP_PID 0xF533

/* Vendor requests (see docs/PROTOCOL.md) */
#define HDCAP_REQ_I2C   0xC0 /* wValue = 8-bit I2C address, wIndex = register, 1 data byte */
#define HDCAP_REQ_GPIO  0xC1 /* wValue = (op << 8) | pin */

/* I2C addresses (8-bit) of the chips behind the FX3 */
#define HDCAP_I2C_MST3367 0x9C /* HDMI receiver + VGA/Component ADC */
#define HDCAP_I2C_TW9900  0x88 /* composite/S-Video decoder */
#define HDCAP_I2C_CS53L21 0x94 /* stereo audio ADC */
#define HDCAP_I2C_CPLD    0x98 /* board glue logic (Altera MAX II) */

typedef struct hdcap hdcap;

/* Error codes: negative values are libusb errors passed through; these are ours. */
#define HDCAP_ERR_NOT_FOUND  (-1000)
#define HDCAP_ERR_PARSE      (-1001)
#define HDCAP_ERR_NOMEM      (-1002)

/* Open the first attached USB3HDCAP and claim its interfaces. */
int  hdcap_open(hdcap **out);
void hdcap_close(hdcap *dev);
const char *hdcap_strerror(int err);

/* Low-level control */
int hdcap_vendor_out(hdcap *dev, uint8_t req, uint16_t value, uint16_t index);
int hdcap_i2c_write(hdcap *dev, uint8_t addr, uint8_t reg, uint8_t val);
int hdcap_i2c_read(hdcap *dev, uint8_t addr, uint8_t reg, uint8_t *val);
int hdcap_set_alt(hdcap *dev, int iface, int alt);

/*
 * Run a sequence file (format: sequences/README.md). Returns 0 or a negative error.
 * If verbose is non-zero, each operation is printed to stderr.
 */
int hdcap_run_sequence(hdcap *dev, const char *path, int verbose);

/* ---------- Stream demultiplexing ---------- */

typedef struct {
    int      width;        /* pixels per line (bytes / 2) */
    int      height;       /* lines in the delivered frame */
    int      interlaced;   /* 1 if woven from two fields */
    uint64_t index;        /* frame counter */
    const uint8_t *yuyv;   /* width * height * 2 bytes, packed YUYV */
} hdcap_frame;

typedef void (*hdcap_frame_cb)(const hdcap_frame *frame, void *user);
/* samples: interleaved int16 L,R; count = number of stereo frames. 48 kHz. */
typedef void (*hdcap_audio_cb)(const int16_t *samples, size_t count, void *user);

typedef struct hdcap_demux hdcap_demux;

hdcap_demux *hdcap_demux_new(hdcap_frame_cb on_frame, hdcap_audio_cb on_audio, void *user);
void hdcap_demux_free(hdcap_demux *dx);
/* Feed raw isochronous payload bytes (any chunking). */
void hdcap_demux_feed(hdcap_demux *dx, const uint8_t *data, size_t len);

typedef struct {
    uint64_t bytes, video_lines, audio_records, audio_frames, frames, resyncs, dropped_frames;
    int last_line_bytes;
} hdcap_demux_stats;
void hdcap_demux_get_stats(const hdcap_demux *dx, hdcap_demux_stats *st);

/* ---------- Streaming ---------- */

/* Start isochronous streaming (selects IF0 alt 1). Payload is fed to the given demuxer. */
int  hdcap_stream_start(hdcap *dev, hdcap_demux *dx);
/* Pump USB events; call repeatedly. timeout_ms <= 0 means non-blocking. */
int  hdcap_stream_poll(hdcap *dev, int timeout_ms);
void hdcap_stream_stop(hdcap *dev);

typedef struct {
    uint64_t transfers, packets, empty_packets, error_packets, bytes;
} hdcap_stream_stats;
void hdcap_stream_get_stats(const hdcap *dev, hdcap_stream_stats *st);

/* ---------- Helpers ---------- */

/* Convert packed YUYV (BT.601 limited range) to RGB24. */
void hdcap_yuyv_to_rgb24(const uint8_t *yuyv, uint8_t *rgb, int width, int height);

#ifdef __cplusplus
}
#endif
#endif
