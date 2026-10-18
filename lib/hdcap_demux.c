/*
 * Stream demultiplexer for the USB3HDCAP isochronous payload.
 *
 * The concatenated payload is a sequence of records (docs/PROTOCOL.md):
 *   FF 00 00 XY  + video line (packed YUYV)    XY = BT.656 status byte (F = bit 6, V = bit 5)
 *   FF 00 FF N   + N * 4 bytes of audio         N stereo frames of int16 LE (L, R), 48 kHz
 * Video lines carry no length field. Their length is found by scanning for the next 0xFF:
 * BT.601 limited-range video never contains 0xFF, while audio records have an explicit length.
 */
#include "hdcap.h"

#include <stdlib.h>
#include <string.h>

#define MAX_LINE_BYTES  8192   /* 4096 px */
#define MAX_FIELD_LINES 1200

enum state { S_SYNC, S_HDR0, S_TYPE, S_ARG, S_VIDEO, S_AUDIO };

struct hdcap_demux {
    hdcap_frame_cb on_frame;
    hdcap_audio_cb on_audio;
    void *user;
    enum state st;
    uint8_t type, arg;
    /* current line */
    uint8_t line[MAX_LINE_BYTES];
    int     line_len, line_overflow;
    /* audio record */
    uint8_t audio[4 * 255];
    int     audio_need, audio_have;
    /* current field: lines stored back to back, all with the same length */
    uint8_t *field;
    int      field_lines, field_line_bytes, field_bad, field_id, in_active;
    int      seen_blank;            /* first (partial) field is discarded */
    /* interlace handling */
    uint8_t *field0;
    int      field0_lines, field0_line_bytes, have_field0;
    uint64_t f1_seen_at;            /* frame counter when a field-1 line was last seen */
    uint8_t *frame;
    hdcap_demux_stats stats;
};

hdcap_demux *hdcap_demux_new(hdcap_frame_cb on_frame, hdcap_audio_cb on_audio, void *user)
{
    hdcap_demux *dx = calloc(1, sizeof(*dx));
    if (!dx)
        return NULL;
    dx->on_frame = on_frame;
    dx->on_audio = on_audio;
    dx->user = user;
    dx->field  = malloc((size_t)MAX_LINE_BYTES * MAX_FIELD_LINES);
    dx->field0 = malloc((size_t)MAX_LINE_BYTES * MAX_FIELD_LINES);
    dx->frame  = malloc((size_t)MAX_LINE_BYTES * MAX_FIELD_LINES * 2);
    if (!dx->field || !dx->field0 || !dx->frame) {
        hdcap_demux_free(dx);
        return NULL;
    }
    return dx;
}

void hdcap_demux_free(hdcap_demux *dx)
{
    if (!dx)
        return;
    free(dx->field);
    free(dx->field0);
    free(dx->frame);
    free(dx);
}

void hdcap_demux_get_stats(const hdcap_demux *dx, hdcap_demux_stats *st)
{
    *st = dx->stats;
}

static void emit(hdcap_demux *dx, const uint8_t *data, int width, int height, int interlaced)
{
    hdcap_frame fr = { width, height, interlaced, dx->stats.frames, data };
    dx->stats.frames++;
    if (dx->on_frame)
        dx->on_frame(&fr, dx->user);
}

/* A run of active lines just ended. */
static void field_done(hdcap_demux *dx)
{
    int lines = dx->field_lines, lb = dx->field_line_bytes;
    dx->field_lines = 0;
    if (!dx->seen_blank || dx->field_bad || lines == 0) {
        if (dx->seen_blank && lines)
            dx->stats.dropped_frames++;
        dx->field_bad = 0;
        return;
    }
    int interlaced = dx->stats.frames < dx->f1_seen_at + 4 && dx->f1_seen_at != 0;
    if (!interlaced) {
        emit(dx, dx->field, lb / 2, lines, 0);
        return;
    }
    if (dx->field_id == 0) {
        memcpy(dx->field0, dx->field, (size_t)lines * lb);
        dx->field0_lines = lines;
        dx->field0_line_bytes = lb;
        dx->have_field0 = 1;
        return;
    }
    if (!dx->have_field0 || dx->field0_line_bytes != lb) {
        dx->stats.dropped_frames++;
        return;
    }
    /* Weave: field 0 on even lines, field 1 on odd lines. */
    int n = dx->field0_lines < lines ? dx->field0_lines : lines;
    for (int i = 0; i < n; i++) {
        memcpy(dx->frame + (size_t)(2 * i) * lb, dx->field0 + (size_t)i * lb, lb);
        memcpy(dx->frame + (size_t)(2 * i + 1) * lb, dx->field + (size_t)i * lb, lb);
    }
    dx->have_field0 = 0;
    emit(dx, dx->frame, lb / 2, 2 * n, 1);
}

static void line_done(hdcap_demux *dx)
{
    uint8_t xy = dx->arg;
    int active = !(xy & 0x20);
    int fid = (xy >> 6) & 1;
    dx->stats.video_lines++;
    dx->stats.last_line_bytes = dx->line_len;
    if (fid)
        dx->f1_seen_at = dx->stats.frames + 1;
    if (!active) {
        if (dx->in_active)
            field_done(dx);
        dx->in_active = 0;
        dx->seen_blank = 1;
        return;
    }
    if (!dx->in_active) {
        dx->in_active = 1;
        dx->field_id = fid;
        dx->field_lines = 0;
        dx->field_bad = 0;
        dx->field_line_bytes = dx->line_len;
    }
    if (dx->line_overflow || dx->line_len != dx->field_line_bytes || (dx->line_len & 3) ||
        dx->field_lines >= MAX_FIELD_LINES) {
        dx->field_bad = 1;
        return;
    }
    memcpy(dx->field + (size_t)dx->field_lines * dx->line_len, dx->line, dx->line_len);
    dx->field_lines++;
}

void hdcap_demux_feed(hdcap_demux *dx, const uint8_t *p, size_t len)
{
    dx->stats.bytes += len;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = p[i];
        switch (dx->st) {
        case S_SYNC:
            if (b == 0xFF)
                dx->st = S_HDR0;
            break;
        case S_HDR0:
            if (b == 0x00) {
                dx->st = S_TYPE;
            } else {
                dx->stats.resyncs++;
                dx->st = b == 0xFF ? S_HDR0 : S_SYNC;
            }
            break;
        case S_TYPE:
            if (b == 0x00 || b == 0xFF) {
                dx->type = b;
                dx->st = S_ARG;
            } else {
                dx->stats.resyncs++;
                dx->st = S_SYNC;
            }
            break;
        case S_ARG:
            dx->arg = b;
            if (dx->type == 0x00) {
                dx->line_len = 0;
                dx->line_overflow = 0;
                dx->st = S_VIDEO;
            } else {
                dx->audio_need = 4 * b;
                dx->audio_have = 0;
                dx->st = dx->audio_need ? S_AUDIO : S_SYNC;
            }
            break;
        case S_VIDEO: {
            /* Fast path: copy up to the next 0xFF, which starts the next record. */
            const uint8_t *s = p + i;
            const uint8_t *e = memchr(s, 0xFF, len - i);
            size_t n = e ? (size_t)(e - s) : len - i;
            if (dx->line_len + n <= MAX_LINE_BYTES) {
                memcpy(dx->line + dx->line_len, s, n);
                dx->line_len += (int)n;
            } else {
                dx->line_overflow = 1;
            }
            if (e) {
                line_done(dx);
                dx->st = S_HDR0;
                i += n;     /* points at the 0xFF; loop increment consumes it */
            } else {
                i = len;
            }
            break;
        }
        case S_AUDIO: {
            size_t n = (size_t)(dx->audio_need - dx->audio_have);
            if (n > len - i)
                n = len - i;
            memcpy(dx->audio + dx->audio_have, p + i, n);
            dx->audio_have += (int)n;
            i += n - 1;
            if (dx->audio_have == dx->audio_need) {
                dx->stats.audio_records++;
                dx->stats.audio_frames += dx->audio_need / 4;
                if (dx->on_audio)
                    dx->on_audio((const int16_t *)dx->audio, (size_t)dx->audio_need / 4, dx->user);
                dx->st = S_SYNC;
            }
            break;
        }
        }
    }
}

void hdcap_yuyv_to_rgb24(const uint8_t *in, uint8_t *out, int width, int height)
{
    size_t pairs = (size_t)width * height / 2;
    for (size_t i = 0; i < pairs; i++, in += 4) {
        int u = in[1] - 128, v = in[3] - 128;
        int rv = 409 * v, gu = -100 * u - 208 * v, bu = 516 * u;
        for (int k = 0; k < 2; k++) {
            int c = 298 * (in[k * 2] - 16) + 128;
            int r = (c + rv) >> 8, g = (c + gu) >> 8, b = (c + bu) >> 8;
            *out++ = r < 0 ? 0 : r > 255 ? 255 : r;
            *out++ = g < 0 ? 0 : g > 255 ? 255 : g;
            *out++ = b < 0 ? 0 : b > 255 ? 255 : b;
        }
    }
}
