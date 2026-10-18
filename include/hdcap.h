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

/* USB link speed in Mbit/s (5000 for SuperSpeed, 480 for High Speed). Video above ~24 MB/s
 * (HDMI, VGA, Component progressive) needs SuperSpeed. */
int hdcap_link_speed_mbps(const hdcap *dev);

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
/* Same, but every `delay` is capped at max_delay_ms (negative = no cap). */
int hdcap_run_sequence_ex(hdcap *dev, const char *path, int verbose, int max_delay_ms);

/* ---------- Inputs and signal ---------- */

typedef enum {
    HDCAP_INPUT_HDMI,       /* HDMI / DVI-D (MST3367 digital) */
    HDCAP_INPUT_VGA,        /* VGA / DVI-A RGB (MST3367 ADC) */
    HDCAP_INPUT_COMPONENT,  /* YPbPr (MST3367 ADC) */
    HDCAP_INPUT_COMPOSITE,  /* CVBS (TW9900) */
    HDCAP_INPUT_SVIDEO,     /* Y/C (TW9900) */
    HDCAP_INPUT_DVI         /* DVI-D on the DVI-I connector (MST3367 second TMDS port) */
} hdcap_input;
#define HDCAP_INPUT_COUNT 6

typedef struct {
    int         locked;      /* stable signal present */
    int         interlaced;
    int         vtotal;      /* lines per frame (per field for interlaced MST3367 sources) */
    double      hfreq_hz;
    double      vfreq_hz;    /* frame (or field) rate */
    const char *standard;    /* analog SD standard (TW9900 inputs), else NULL */
    uint8_t     raw[5];      /* status registers, for debugging */
} hdcap_signal;

const char *hdcap_input_name(hdcap_input in);
/* One-time device init after open: fixed requests and capability probes. */
int hdcap_init(hdcap *dev);
/* Reset the front end and configure every chip for the given input (blocks for ~0.3-3 s). */
int hdcap_select_input(hdcap *dev, hdcap_input in);
/* Read the current signal state for the selected input. */
int hdcap_get_signal(hdcap *dev, hdcap_input in, hdcap_signal *s);
/* Poll until a stable signal is seen (returns 1), the timeout passes (0), or an error (<0). */
int hdcap_wait_signal(hdcap *dev, hdcap_input in, int timeout_ms, hdcap_signal *s);

/* ---------- Analog timing modes (VGA / Component) ---------- */

typedef struct {
    const char *name;
    int    htotal, hactive, hstart;   /* samples per line; hstart = sync + back porch */
    int    vtotal, vactive, vstart;   /* lines per frame; vactive/vstart per field if interlaced */
    double pclk_mhz;
    int    interlaced;
    int    family;                    /* 0 = VESA/PC, 1 = CEA-861 video */
} hdcap_mode;

const hdcap_mode *hdcap_modes(size_t *count);
/* Best standard mode for a locked analog signal, or NULL. */
const hdcap_mode *hdcap_find_mode(hdcap_input in, const hdcap_signal *s);
/* Program the MST3367 sampling PLL (h-total) and crop window to the mode's active area,
 * shifted by the given offsets (samples / lines). */
int hdcap_set_mode(hdcap *dev, const hdcap_mode *m, int hoffset, int voffset);

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

/* Start isochronous streaming. Selects IF0 alt 1 on SuperSpeed (16 KiB per 125 us) or alt 2 on
 * High Speed (3 KiB per 125 us, the most USB 2.0 allows). Payload is fed to the given demuxer. */
int  hdcap_stream_start(hdcap *dev, hdcap_demux *dx);
/* Same, choosing the alt setting for an expected stream rate in bytes/s (0 = unknown -> largest).
 * SuperSpeed: alt 1 carries ~131 MB/s, alt 2 ~262 MB/s. High Speed: alt 2 (~24.6 MB/s). */
int  hdcap_stream_start_rate(hdcap *dev, hdcap_demux *dx, double bytes_per_sec);
/* Expected stream rate for a locked signal: every line (blanking included) is sent at the active
 * width, so rate = line rate x (2 x active width + header) plus audio. 0 if the width is unknown. */
double hdcap_estimate_rate(hdcap_input in, const hdcap_signal *s, const hdcap_mode *analog_mode);
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
