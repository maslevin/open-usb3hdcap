/*
 * USB access for the USB3HDCAP: device open, vendor requests, sequence files, isochronous streaming.
 */
#include "hdcap.h"

#include <libusb.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CTRL_TIMEOUT_MS 1000

#define ISO_EP          0x83
#define ISO_IFACE       0
#define ISO_ALT         1
#define ISO_TRANSFERS   8
#define ISO_PACKETS     32

struct hdcap {
    libusb_context       *ctx;
    libusb_device_handle *h;
    /* streaming */
    struct libusb_transfer *xfer[ISO_TRANSFERS];
    unsigned char          *buf[ISO_TRANSFERS];
    int                     pkt_size;
    int                     streaming;
    int                     in_flight;
    hdcap_demux            *dx;
    hdcap_stream_stats      stats;
};

const char *hdcap_strerror(int err)
{
    switch (err) {
    case HDCAP_ERR_NOT_FOUND: return "USB3HDCAP (1164:F533) not found";
    case HDCAP_ERR_PARSE:     return "sequence file parse error";
    case HDCAP_ERR_NOMEM:     return "out of memory";
    default:                  return libusb_error_name(err);
    }
}

int hdcap_open(hdcap **out)
{
    hdcap *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return HDCAP_ERR_NOMEM;
    int r = libusb_init(&dev->ctx);
    if (r < 0)
        goto fail;
    dev->h = libusb_open_device_with_vid_pid(dev->ctx, HDCAP_VID, HDCAP_PID);
    if (!dev->h) {
        r = HDCAP_ERR_NOT_FOUND;
        goto fail;
    }
    libusb_set_auto_detach_kernel_driver(dev->h, 1);
    /* macOS leaves vendor-class devices unconfigured when no driver matches; select configuration 1. */
    int cfg = 0;
    if (libusb_get_configuration(dev->h, &cfg) == 0 && cfg != 1 &&
        (r = libusb_set_configuration(dev->h, 1)) < 0)
        goto fail;
    if ((r = libusb_claim_interface(dev->h, 0)) < 0)
        goto fail;
    if ((r = libusb_claim_interface(dev->h, 1)) < 0)
        goto fail;
    *out = dev;
    return 0;
fail:
    hdcap_close(dev);
    return r;
}

void hdcap_close(hdcap *dev)
{
    if (!dev)
        return;
    if (dev->streaming)
        hdcap_stream_stop(dev);
    if (dev->h) {
        libusb_release_interface(dev->h, 1);
        libusb_release_interface(dev->h, 0);
        libusb_close(dev->h);
    }
    if (dev->ctx)
        libusb_exit(dev->ctx);
    free(dev);
}

int hdcap_vendor_out(hdcap *dev, uint8_t req, uint16_t value, uint16_t index)
{
    int r = libusb_control_transfer(dev->h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT,
                                    req, value, index, NULL, 0, CTRL_TIMEOUT_MS);
    return r < 0 ? r : 0;
}

int hdcap_i2c_write(hdcap *dev, uint8_t addr, uint8_t reg, uint8_t val)
{
    int r = libusb_control_transfer(dev->h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT,
                                    HDCAP_REQ_I2C, addr, reg, &val, 1, CTRL_TIMEOUT_MS);
    return r < 0 ? r : 0;
}

int hdcap_i2c_read(hdcap *dev, uint8_t addr, uint8_t reg, uint8_t *val)
{
    int r = libusb_control_transfer(dev->h, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN,
                                    HDCAP_REQ_I2C, addr, reg, val, 1, CTRL_TIMEOUT_MS);
    return r < 0 ? r : 0;
}

int hdcap_set_alt(hdcap *dev, int iface, int alt)
{
    return libusb_set_interface_alt_setting(dev->h, iface, alt);
}

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

/* ---------- sequence files ---------- */

int hdcap_run_sequence(hdcap *dev, const char *path, int verbose)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        perror(path);
        return HDCAP_ERR_PARSE;
    }
    char line[512];
    int lineno = 0, rc = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        char op[16];
        unsigned a = 0, b = 0, c = 0;
        int n = sscanf(line, "%15s %i %i %i", op, (int *)&a, (int *)&b, (int *)&c);
        if (n <= 0)
            continue; /* blank or comment */
        int r = 0;
        if (!strcmp(op, "i2c_w") && n == 4) {
            r = hdcap_i2c_write(dev, a, b, c);
        } else if (!strcmp(op, "i2c_r") && n == 3) {
            uint8_t v;
            r = hdcap_i2c_read(dev, a, b, &v);
            if (verbose && !r)
                fprintf(stderr, "  read %02x:%02x = %02x\n", a, b, v);
        } else if (!strcmp(op, "vreq") && n == 4) {
            r = hdcap_vendor_out(dev, a, b, c);
            /* Capability probes (0xC5) are expected to stall on this model. */
            if (r == LIBUSB_ERROR_PIPE) {
                if (verbose)
                    fprintf(stderr, "  vreq %02x stalled (ignored)\n", a);
                r = 0;
            }
        } else if (!strcmp(op, "alt") && n == 3) {
            r = hdcap_set_alt(dev, a, b);
        } else if (!strcmp(op, "delay") && n == 2) {
            sleep_ms(a);
        } else {
            fprintf(stderr, "%s:%d: cannot parse: %s\n", path, lineno, line);
            rc = HDCAP_ERR_PARSE;
            break;
        }
        if (verbose > 1)
            fprintf(stderr, "%s:%d %s", path, lineno, line);
        if (r < 0) {
            fprintf(stderr, "%s:%d: %s failed: %s\n", path, lineno, op, hdcap_strerror(r));
            rc = r;
            break;
        }
    }
    fclose(f);
    return rc;
}

/* ---------- isochronous streaming ---------- */

static void LIBUSB_CALL iso_cb(struct libusb_transfer *t)
{
    hdcap *dev = t->user_data;
    dev->stats.transfers++;
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i < t->num_iso_packets; i++) {
            struct libusb_iso_packet_descriptor *p = &t->iso_packet_desc[i];
            dev->stats.packets++;
            if (p->status != LIBUSB_TRANSFER_COMPLETED) {
                dev->stats.error_packets++;
                continue;
            }
            if (!p->actual_length) {
                dev->stats.empty_packets++;
                continue;
            }
            dev->stats.bytes += p->actual_length;
            hdcap_demux_feed(dev->dx, libusb_get_iso_packet_buffer_simple(t, i), p->actual_length);
        }
    }
    if (dev->streaming && t->status != LIBUSB_TRANSFER_CANCELLED && t->status != LIBUSB_TRANSFER_NO_DEVICE) {
        if (libusb_submit_transfer(t) == 0)
            return;
    }
    dev->in_flight--;
}

int hdcap_stream_start(hdcap *dev, hdcap_demux *dx)
{
    int r = hdcap_set_alt(dev, ISO_IFACE, ISO_ALT);
    if (r < 0)
        return r;
    /* Per-interval size for the streaming alt setting (wMaxPacketSize x burst x mult; 16 KiB for alt 1).
     * libusb_get_max_iso_packet_size() would report alt setting 0, which has zero bandwidth. */
    dev->pkt_size = libusb_get_max_alt_packet_size(libusb_get_device(dev->h), ISO_IFACE, ISO_ALT, ISO_EP);
    if (dev->pkt_size <= 0)
        return dev->pkt_size ? dev->pkt_size : LIBUSB_ERROR_OTHER;
    dev->dx = dx;
    dev->streaming = 1;
    memset(&dev->stats, 0, sizeof dev->stats);
    for (int i = 0; i < ISO_TRANSFERS; i++) {
        size_t len = (size_t)dev->pkt_size * ISO_PACKETS;
        dev->buf[i] = malloc(len);
        dev->xfer[i] = libusb_alloc_transfer(ISO_PACKETS);
        if (!dev->buf[i] || !dev->xfer[i])
            return HDCAP_ERR_NOMEM;
        libusb_fill_iso_transfer(dev->xfer[i], dev->h, ISO_EP, dev->buf[i], (int)len, ISO_PACKETS, iso_cb, dev, 1000);
        libusb_set_iso_packet_lengths(dev->xfer[i], dev->pkt_size);
        if ((r = libusb_submit_transfer(dev->xfer[i])) < 0) {
            hdcap_stream_stop(dev);
            return r;
        }
        dev->in_flight++;
    }
    return 0;
}

int hdcap_stream_poll(hdcap *dev, int timeout_ms)
{
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    if (timeout_ms <= 0)
        tv.tv_sec = tv.tv_usec = 0;
    return libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
}

void hdcap_stream_stop(hdcap *dev)
{
    dev->streaming = 0;
    for (int i = 0; i < ISO_TRANSFERS; i++)
        if (dev->xfer[i])
            libusb_cancel_transfer(dev->xfer[i]);
    for (int spins = 0; dev->in_flight > 0 && spins < 200; spins++) {
        struct timeval tv = { 0, 10000 };
        libusb_handle_events_timeout_completed(dev->ctx, &tv, NULL);
    }
    for (int i = 0; i < ISO_TRANSFERS; i++) {
        if (dev->xfer[i])
            libusb_free_transfer(dev->xfer[i]);
        free(dev->buf[i]);
        dev->xfer[i] = NULL;
        dev->buf[i] = NULL;
    }
    dev->in_flight = 0;
    hdcap_set_alt(dev, ISO_IFACE, 0);
}

void hdcap_stream_get_stats(const hdcap *dev, hdcap_stream_stats *st)
{
    *st = dev->stats;
}
