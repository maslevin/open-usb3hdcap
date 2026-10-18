/* SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2026 Matt Slevinsky */
/* Register tables for input selection (generated: lib/hdcap_tables.c, tools/gen_tables.py). */
#ifndef HDCAP_TABLES_H
#define HDCAP_TABLES_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t  addr;    /* 8-bit I2C address */
    uint8_t  bank;    /* MST3367 bank (written to reg 0x00 first), 0xFF if not banked */
    uint8_t  reg;
    uint8_t  val;
    uint16_t delay_ms;
} hdcap_reg_write;

extern const hdcap_reg_write hdcap_table_hdmi[], hdcap_table_vga[], hdcap_table_component[],
                             hdcap_table_composite[], hdcap_table_svideo[];
extern const size_t hdcap_table_hdmi_len, hdcap_table_vga_len, hdcap_table_component_len,
                    hdcap_table_composite_len, hdcap_table_svideo_len;
#endif
