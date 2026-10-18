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

int hdcap_get_signal(hdcap *dev, hdcap_input in, hdcap_signal *s)
{
    memset(s, 0, sizeof *s);
    int r;
    if (in == HDCAP_INPUT_COMPOSITE || in == HDCAP_INPUT_SVIDEO) {
        /* TW9900 STATUS1 (0x01): bit7 VDLOSS, bit6 HLOCK, bit3 VLOCK, bit0 DET50. SDT (0x1C) bits 6-4: standard. */
        static const char *const std[] = { "NTSC-M", "PAL-BDGHI", "SECAM", "NTSC-4.43", "PAL-M", "PAL-CN", "PAL-60", "?" };
        uint8_t st, sdt;
        if ((r = hdcap_i2c_read(dev, HDCAP_I2C_TW9900, 0x01, &st)) < 0)
            return r;
        if ((r = hdcap_i2c_read(dev, HDCAP_I2C_TW9900, 0x1C, &sdt)) < 0)
            return r;
        s->raw[0] = st;
        s->raw[1] = sdt;
        s->locked = !(st & 0x80) && (st & 0x40) && (st & 0x08);
        s->interlaced = 1;
        s->vfreq_hz = (st & 0x01) ? 50.0 : 59.94;
        s->vtotal = (st & 0x01) ? 625 : 525;
        s->hfreq_hz = s->vtotal * s->vfreq_hz / 2;
        s->standard = s->locked ? std[(sdt >> 4) & 7] : NULL;
        return 0;
    }
    /* MST3367 bank 0 measurements: 0x57/0x58 = line period (12 bits, reference-clock counts),
     * 0x5B/0x5C = lines per frame (or per field when interlaced). 0x55 = HDMI/ADC status. */
    uint8_t p_hi, p_lo, v_hi, v_lo, st;
    if ((r = mst_read(dev, 0x00, 0x57, &p_hi)) < 0 || (r = mst_read(dev, 0x00, 0x58, &p_lo)) < 0 ||
        (r = mst_read(dev, 0x00, 0x5B, &v_hi)) < 0 || (r = mst_read(dev, 0x00, 0x5C, &v_lo)) < 0 ||
        (r = mst_read(dev, 0x00, 0x55, &st)) < 0)
        return r;
    int period = ((p_hi & 0x1F) << 8) | p_lo;
    int vtotal = ((v_hi & 0x07) << 8) | v_lo;
    s->raw[0] = st;
    s->raw[1] = p_hi;
    s->raw[2] = p_lo;
    s->raw[3] = v_hi;
    s->raw[4] = v_lo;
    if (period > 0 && period < 0x1FFF && vtotal > 100 && vtotal < 1400) {
        s->hfreq_hz = MST_MEAS_CLOCK_HZ / period;
        s->vtotal = vtotal;
        s->vfreq_hz = s->hfreq_hz / vtotal;
        /* An interlaced source reports lines per field (262/263, 312/313, 562/563). */
        s->interlaced = (vtotal == 262 || vtotal == 263 || vtotal == 312 || vtotal == 313 ||
                         vtotal == 562 || vtotal == 563);
        s->locked = s->vfreq_hz > 20 && s->vfreq_hz < 200;
    }
    s->standard = NULL;
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
