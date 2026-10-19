/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/*
 * Device initialisation, input selection and signal detection.
 *
 * Register meanings come from the TW9900 and CS53L21 datasheets and from observed behaviour of the
 * MST3367 and CPLD (docs/PROTOCOL.md). Per-input register tables are in hdcap_tables.c.
 */
#include "hdcap.h"
#include "hdcap_tables.h"

#include <libusb.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* MST3367 measurement reference clock, derived from captured line periods:
 * 3559 counts at 45.000 kHz (720p) and 5089 counts at 31.469 kHz (640x480). */
#define MST_MEAS_CLOCK_HZ 160150000.0

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

const char *hdcap_input_name(hdcap_input in)
{
    switch (in) {
    case HDCAP_INPUT_HDMI:      return "HDMI";
    case HDCAP_INPUT_VGA:       return "VGA";
    case HDCAP_INPUT_COMPONENT: return "Component";
    case HDCAP_INPUT_COMPOSITE: return "Composite";
    case HDCAP_INPUT_SVIDEO:    return "S-Video";
    case HDCAP_INPUT_DVI:       return "DVI";
    }
    return "?";
}

int hdcap_init(hdcap *dev)
{
    /* Fixed init requests (meaning not yet known) and capability probes; 0xC5 stalls on F533. */
    int r;
    if ((r = hdcap_vendor_out(dev, 0xC2, 0, 0)) < 0)
        return r;
    if ((r = hdcap_vendor_out(dev, 0xC7, 100, 0)) < 0)
        return r;
    r = hdcap_vendor_out(dev, 0xC5, 0, 0);
    if (r < 0 && r != LIBUSB_ERROR_PIPE)
        return r;
    r = hdcap_vendor_out(dev, 0xC6, 0, 0);
    if (r < 0 && r != LIBUSB_ERROR_PIPE)
        return r;
    return 0;
}

/* 50 ms low pulse on FX3 GPIO 45 (MST3367 reset), as issued before every input switch. */
static int gpio45_reset(hdcap *dev)
{
    static const uint16_t seq[] = { 0x402D, 0x012D, 0x002D, 0x012D, 0x802D };
    for (size_t i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        int r = hdcap_vendor_out(dev, HDCAP_REQ_GPIO, seq[i], 0);
        if (r < 0)
            return r;
        if (i >= 1 && i <= 3)
            sleep_ms(50);
    }
    return 0;
}

/* DVI-D uses the HDMI setup with two differences (capture c18): MST3367 0x51 bit 3 selects the
 * second TMDS port (0x81 -> 0x89) and the CPLD routes it with reg 0x01 = 0x01 (HDMI: 0x02). */
static uint8_t dvi_patch(const hdcap_reg_write *w)
{
    if (w->addr == HDCAP_I2C_MST3367 && w->bank == 0x00 && w->reg == 0x51 && w->val == 0x81)
        return 0x89;
    if (w->addr == HDCAP_I2C_CPLD && w->reg == 0x01 && w->val == 0x02)
        return 0x01;
    return w->val;
}

static int apply_table(hdcap *dev, const hdcap_reg_write *t, size_t n, int dvi)
{
    int bank = -1;
    for (size_t i = 0; i < n; i++) {
        int r;
        if (t[i].delay_ms)
            sleep_ms(t[i].delay_ms);
        if (t[i].bank != 0xFF && t[i].bank != bank) {
            if ((r = hdcap_i2c_write(dev, HDCAP_I2C_MST3367, 0x00, t[i].bank)) < 0)
                return r;
            bank = t[i].bank;
        }
        if ((r = hdcap_i2c_write(dev, t[i].addr, t[i].reg, dvi ? dvi_patch(&t[i]) : t[i].val)) < 0)
            return r;
    }
    return 0;
}

int hdcap_select_input(hdcap *dev, hdcap_input in)
{
    const hdcap_reg_write *t;
    size_t n;
    switch (in) {
    case HDCAP_INPUT_HDMI:
    case HDCAP_INPUT_DVI:       t = hdcap_table_hdmi;      n = hdcap_table_hdmi_len;      break;
    case HDCAP_INPUT_VGA:       t = hdcap_table_vga;       n = hdcap_table_vga_len;       break;
    case HDCAP_INPUT_COMPONENT: t = hdcap_table_component; n = hdcap_table_component_len; break;
    case HDCAP_INPUT_COMPOSITE: t = hdcap_table_composite; n = hdcap_table_composite_len; break;
    case HDCAP_INPUT_SVIDEO:    t = hdcap_table_svideo;    n = hdcap_table_svideo_len;    break;
    default: return HDCAP_ERR_PARSE;
    }
    int r = gpio45_reset(dev);
    if (r < 0)
        return r;
    return apply_table(dev, t, n, in == HDCAP_INPUT_DVI);
}

static int mst_read(hdcap *dev, uint8_t bank, uint8_t reg, uint8_t *v)
{
    int r = hdcap_i2c_write(dev, HDCAP_I2C_MST3367, 0x00, bank);
    return r < 0 ? r : hdcap_i2c_read(dev, HDCAP_I2C_MST3367, reg, v);
}

/* Decode the MST3367 timing registers { 0x55, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5F } (pure: also
 * used by tests/mst_decode_test.c on values read by the Windows driver). */
void hdcap__mst_decode(const uint8_t raw[8], hdcap_signal *s)
{
    memcpy(s->raw, raw, 8);
    int period = ((raw[1] & 0x3F) << 8) | raw[2];
    int fperiod = ((raw[3] & 0x3F) << 8) | raw[4];
    int lines = ((raw[5] & 0x07) << 8) | raw[6];
    if (period <= 0 || period >= 0x3FFF || fperiod <= 0 || fperiod >= 0x3FFF)
        return;     /* counters saturate without a signal */
    s->hfreq_hz = MST_MEAS_CLOCK_HZ / period;
    double field_hz = MST_MEAS_CLOCK_HZ / 1280.0 / fperiod;
    s->interlaced = (raw[7] & 0x02) != 0;
    if (s->interlaced) {
        /* lines per field from the two rates: frame lines are odd (525, 625, 1125), so round
         * the frame and halve, giving the 262/312/562 the mode table expects */
        s->vfreq_hz = field_hz;
        s->vtotal = (int)(2.0 * s->hfreq_hz / field_hz + 0.5) / 2;
    } else {
        /* the line count is exact; the field counter must agree (rejects transients) */
        if (lines <= 100 || lines >= 1400)
            return;
        s->vtotal = lines;
        s->vfreq_hz = s->hfreq_hz / lines;
        if (fabs(s->vfreq_hz - field_hz) > field_hz * 0.02)
            return;
    }
    s->locked = s->vfreq_hz > 20 && s->vfreq_hz < 200 && s->vtotal > 100;
}

int hdcap_get_signal(hdcap *dev, hdcap_input in, hdcap_signal *s)
{
    memset(s, 0, sizeof *s);
    int r;
    if (in == HDCAP_INPUT_COMPOSITE || in == HDCAP_INPUT_SVIDEO) {
        /* TW9900 STATUS1 (0x01): bit7 VDLOSS, bit6 HLOCK, bit3 VLOCK, bit0 DET50. SDT (0x1C) bits 6-4: standard. */
        static const char *const std[] = { "NTSC-M", "PAL-BDGHI", "SECAM", "NTSC-4.43", "PAL-M", "PAL-CN", "PAL-60", "?" };
        /* STATUS2 (0x31) bit 3 NINTL: non-interlaced source (240p/288p consoles) */
        uint8_t st, sdt, st2;
        if ((r = hdcap_i2c_read(dev, HDCAP_I2C_TW9900, 0x01, &st)) < 0 ||
            (r = hdcap_i2c_read(dev, HDCAP_I2C_TW9900, 0x1C, &sdt)) < 0 ||
            (r = hdcap_i2c_read(dev, HDCAP_I2C_TW9900, 0x31, &st2)) < 0)
            return r;
        s->raw[0] = st;
        s->raw[1] = sdt;
        s->raw[2] = st2;
        s->locked = !(st & 0x80) && (st & 0x40) && (st & 0x08);
        s->interlaced = !(st2 & 0x08);
        s->vfreq_hz = (st & 0x01) ? 50.0 : 59.94;
        /* lines per frame, as for MST3367 sources: per field (262/312) when progressive */
        s->vtotal = (st & 0x01) ? (s->interlaced ? 625 : 312) : (s->interlaced ? 525 : 262);
        s->hfreq_hz = (s->interlaced ? s->vtotal / 2.0 : s->vtotal) * s->vfreq_hz;
        s->standard = s->locked ? std[(sdt >> 4) & 7] : NULL;
        return 0;
    }
    /* MST3367 measurements (bank 0), checked against the Windows driver's polling (captures
     * c05/c11/c14/c15) and hdcapm's MST3367 driver:
     *   0x57[5:0] 0x58  line period, reference-clock counts (14 bits: 480i/576i need bit 13)
     *   0x59[5:0] 0x5A  field period, reference/1280 counts (~125 kHz)
     *   0x5B[2:0] 0x5C  lines per field; analog interlaced sync inflates it (480i reads 271/272:
     *                   equalizing and serration pulses count as lines), so it is only used
     *                   for progressive signals
     *   0x5F bit 1      interlaced
     *   0x55            status
     * HDMI/DVI only: 0x6A[3:0] 0x6B htotal (pixels), bank 2 0x28 0x29[4:0] active width. */
    uint8_t p_hi, p_lo, f_hi, f_lo, v_hi, v_lo, il, st;
    if ((r = mst_read(dev, 0x00, 0x57, &p_hi)) < 0 || (r = mst_read(dev, 0x00, 0x58, &p_lo)) < 0 ||
        (r = mst_read(dev, 0x00, 0x59, &f_hi)) < 0 || (r = mst_read(dev, 0x00, 0x5A, &f_lo)) < 0 ||
        (r = mst_read(dev, 0x00, 0x5B, &v_hi)) < 0 || (r = mst_read(dev, 0x00, 0x5C, &v_lo)) < 0 ||
        (r = mst_read(dev, 0x00, 0x5F, &il)) < 0 || (r = mst_read(dev, 0x00, 0x55, &st)) < 0)
        return r;
    uint8_t raw[8] = { st, p_hi, p_lo, f_hi, f_lo, v_hi, v_lo, il };
    hdcap__mst_decode(raw, s);
    if (s->locked && (in == HDCAP_INPUT_HDMI || in == HDCAP_INPUT_DVI)) {
        uint8_t h_hi, h_lo, a_lo, a_hi;
        if ((r = mst_read(dev, 0x00, 0x6A, &h_hi)) < 0 || (r = mst_read(dev, 0x00, 0x6B, &h_lo)) < 0 ||
            (r = mst_read(dev, 0x02, 0x28, &a_lo)) < 0 || (r = mst_read(dev, 0x02, 0x29, &a_hi)) < 0)
            return r;
        int ht = ((h_hi & 0x0F) << 8) | h_lo, ha = ((a_hi & 0x1F) << 8) | a_lo;
        if (ha > 0 && ht > ha && ht < 4096) {
            s->htotal = ht;
            s->hactive = ha;
        }
    }
    return 0;
}

int hdcap_wait_signal(hdcap *dev, hdcap_input in, int timeout_ms, hdcap_signal *s)
{
    /* Locked = two consecutive matching measurements. */
    hdcap_signal prev = {0};
    for (int waited = 0;; waited += 100) {
        int r = hdcap_get_signal(dev, in, s);
        if (r < 0)
            return r;
        if (s->locked && prev.locked && s->vtotal == prev.vtotal)
            return 1;
        if (waited >= timeout_ms)
            return 0;
        prev = *s;
        sleep_ms(100);
    }
}

/* ---------- Digital inputs: colour matrix and audio source ---------- */

int hdcap_read_avi(hdcap *dev, hdcap_avi *a)
{
    memset(a, 0, sizeof *a);
    /* Bank 2 0x0B is a read-to-acknowledge status register: the receiver only latches a newly
     * received InfoFrame into 0x48-0x4B after it has been read (and the first read after a reset
     * returns a stale value). Read it, give the receiver a frame's time, then read the InfoFrame. */
    uint8_t ack;
    int r0 = mst_read(dev, 0x02, 0x0B, &ack);
    if (r0 < 0)
        return r0;
    sleep_ms(100);
    for (int i = 0; i < 4; i++) {
        int r = mst_read(dev, 0x02, (uint8_t)(0x48 + i), &a->pb[i]);
        if (r < 0)
            return r;
    }
    /* PB1..PB4 all zero = nothing received yet (or a DVI source without InfoFrames). */
    a->valid = a->pb[0] || a->pb[1] || a->pb[2] || a->pb[3];
    a->colorspace = (a->pb[0] >> 5) & 3;
    a->colorimetry = (a->pb[1] >> 6) & 3;
    a->quant_range = (a->pb[2] >> 2) & 3;
    a->vic = a->pb[3] & 0x7F;
    return 0;
}

/* MST3367 bank 0 0x93-0xAA: nine 15-bit two's-complement coefficients (4096 = 1.0), grouped by
 * input channel R, G, B; within a group the outputs are Cr, Y, Cb. Then offsets for Cr, Y, Cb
 * (64 = 1 code; 0x2000 = 128). For YCbCr input the R/G/B slots carry Cr/Y/Cb. */
static int write_csc(hdcap *dev, const double c[9], const int off[3])
{
    int r = hdcap_i2c_write(dev, HDCAP_I2C_MST3367, 0x00, 0x00);
    uint8_t reg = 0x93;
    for (int i = 0; r >= 0 && i < 12; i++, reg += 2) {
        int v = i < 9 ? (int)lround(c[i] * 4096) : off[i - 9];
        v &= 0x7FFF;
        if ((r = hdcap_i2c_write(dev, HDCAP_I2C_MST3367, reg, (uint8_t)(v >> 8))) >= 0)
            r = hdcap_i2c_write(dev, HDCAP_I2C_MST3367, (uint8_t)(reg + 1), (uint8_t)v);
    }
    return r < 0 ? r : 0;
}

static int set_csc_rgb(hdcap *dev, int bt709, int full_range)
{
    double kr = bt709 ? 0.2126 : 0.299, kb = bt709 ? 0.0722 : 0.114, kg = 1 - kr - kb;
    double ys = full_range ? 219.0 / 255 : 1.0, cs = full_range ? 224.0 / 255 : 224.0 / 219;
    double cb_r = -kr / (2 * (1 - kb)) * cs, cb_g = -kg / (2 * (1 - kb)) * cs, cb_b = 0.5 * cs;
    double cr_r = 0.5 * cs, cr_g = -kg / (2 * (1 - kr)) * cs, cr_b = -kb / (2 * (1 - kr)) * cs;
    const double c[9] = { cr_r, kr * ys, cb_r, cr_g, kg * ys, cb_g, cr_b, kb * ys, cb_b };
    /* Limited-range RGB keeps its +16 through the luma row (coefficients sum to 1); full range needs +16. */
    const int off[3] = { 0x2000, full_range ? 16 * 64 : 0, 0x2000 };
    return write_csc(dev, c, off);
}

static int set_csc_passthru(hdcap *dev)
{
    const double c[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    const int off[3] = { 0, 0, 0 };
    return write_csc(dev, c, off);
}

int hdcap_setup_digital(hdcap *dev, int timeout_ms, hdcap_digital_setup *o)
{
    memset(o, 0, sizeof *o);
    int r;
    for (int waited = 0;; waited += 200) { /* each hdcap_read_avi() takes ~100 ms */
        if ((r = hdcap_read_avi(dev, &o->avi)) < 0)
            return r;
        if (o->avi.valid || waited >= timeout_ms)
            break;
        sleep_ms(100);
    }
    const hdcap_avi *a = &o->avi;
    if (a->valid && (a->colorspace == 1 || a->colorspace == 2)) {
        o->ycbcr_passthru = 1;
        o->bt709 = a->colorimetry != 1;
        r = set_csc_passthru(dev);
    } else {
        /* RGB. Range: explicit Q field, else CEA video formats (VIC > 1) are limited and IT/PC
         * formats (VIC 0/1, or no InfoFrame = plain DVI) are full range. */
        o->full_range = a->quant_range == 2 || (a->quant_range == 0 && a->vic <= 1);
        o->bt709 = a->colorimetry == 2 || (a->colorimetry == 0 && a->vic != 2 && a->vic != 3 &&
                                           !(a->vic >= 6 && a->vic <= 15) && !(a->vic >= 17 && a->vic <= 26));
        r = set_csc_rgb(dev, o->bt709, o->full_range);
    }
    if (r < 0)
        return r;
    /* Audio source: embedded when the receiver reports HDMI audio (bank 2 0x0B bits 6:5),
     * otherwise line-in. CPLD 0x20 bit 4 = 1 selects line-in. */
    uint8_t st, cp;
    if ((r = mst_read(dev, 0x02, 0x0B, &st)) < 0 || (r = hdcap_i2c_read(dev, HDCAP_I2C_CPLD, 0x20, &cp)) < 0)
        return r;
    o->embedded_audio = (st & 0x60) == 0x60;
    cp = o->embedded_audio ? (cp & ~0x10) : (cp | 0x10);
    return hdcap_i2c_write(dev, HDCAP_I2C_CPLD, 0x20, cp);
}

/* ---------- SD inputs: TV standard (TW9900 0x1C SDT, 0x1D SDTR) ---------- */

const char *hdcap_std_name(hdcap_std std)
{
    static const char *const n[] = { "NTSC-M", "PAL", "SECAM", "NTSC-4.43", "PAL-M", "PAL-CN", "PAL-60", "Auto" };
    return (unsigned)std < 8 ? n[std] : "?";
}

/* Picture window for 625-line standards. The input tables end with the window the Windows driver
 * sets after detecting NTSC (CROP_HI 0x02, HDELAY 0x10, HACTIVE 0x2D0, VACTIVE 0xF2 = 242 lines per
 * field, VVBI bit 4); for 50 Hz it uses VDELAY 0x19, VACTIVE 0x120 = 288 lines (CROP_HI bits 5:4),
 * HDELAY 0x0A and VVBI bit 4 clear (values from mlaux/usb3hdcap; layout per the TW9900 datasheet,
 * registers 0x07-0x0B and 0x55). Without this a 576i picture is cut to 484 lines. */
static int sd_window_625(hdcap *dev)
{
    uint8_t crop, vvbi;
    int r;
    if ((r = hdcap_i2c_read(dev, HDCAP_I2C_TW9900, 0x07, &crop)) < 0 ||
        (r = hdcap_i2c_read(dev, HDCAP_I2C_TW9900, 0x55, &vvbi)) < 0 ||
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x07, (uint8_t)((crop & 0x0F) | 0x10))) < 0 ||
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x08, 0x19)) < 0 ||
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x09, 0x20)) < 0 ||
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x0A, 0x0A)) < 0 ||
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x0B, 0xD0)) < 0 ||
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x55, (uint8_t)(vvbi & ~0x10))) < 0)
        return r;
    return 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int hdcap_set_picture(hdcap *dev, const hdcap_picture *p)
{
    int r;
    uint8_t sat = (uint8_t)clampi(p->saturation, 0, 255);
    if ((r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x10, (uint8_t)(int8_t)clampi(p->brightness, -128, 127))) < 0 ||
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x11, (uint8_t)clampi(p->contrast, 0, 255))) < 0 ||
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x13, sat)) < 0 ||
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x14, sat)) < 0 ||
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x15, (uint8_t)(int8_t)clampi(p->hue, -128, 127))) < 0 ||
        /* 0x12: keep the Windows upper bits (0x50: VSF, CTI level 1), sharpness in bits 3:0 */
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x12, (uint8_t)(0x50 | clampi(p->sharpness, 0, 15)))) < 0)
        return r;
    return 0;
}

static int std_is_625(int std) { return std == HDCAP_STD_PAL || std == HDCAP_STD_SECAM || std == HDCAP_STD_PAL_CN; }

int hdcap_setup_sd(hdcap *dev, hdcap_std std, int timeout_ms)
{
    int r;
    uint8_t sdt;
    if (std != HDCAP_STD_AUTO) {
        if ((r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x1C, (uint8_t)std)) < 0)
            return r;
        if (std_is_625(std) && (r = sd_window_625(dev)) < 0)
            return r;
        return std;
    }
    /* Auto: detection can latch a wrong standard if it ran while the source was still settling
     * (seen with a Dreamcast after power-up: NTSC detected as PAL-60, colour killed). Restart it
     * now that the signal is locked (SDTR bit 7 ATSTART, keeping all standards enabled) and wait
     * for SDT bit 7 (DETSTUS) to clear. */
    if ((r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x1C, HDCAP_STD_AUTO)) < 0 ||
        (r = hdcap_i2c_write(dev, HDCAP_I2C_TW9900, 0x1D, 0xFF)) < 0)
        return r;
    for (int waited = 0;; waited += 50) {
        sleep_ms(50);
        if ((r = hdcap_i2c_read(dev, HDCAP_I2C_TW9900, 0x1C, &sdt)) < 0)
            return r;
        if (!(sdt & 0x80) && waited >= 200)
            break;
        if (waited >= timeout_ms)
            break;
    }
    int detected = (sdt >> 4) & 7;
    if (std_is_625(detected) && (r = sd_window_625(dev)) < 0)
        return r;
    return detected;
}
