# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=c11
CFLAGS  += -Iinclude $(shell pkg-config --cflags libusb-1.0)
LDLIBS  += $(shell pkg-config --libs libusb-1.0)

LIB_SRC = lib/hdcap_usb.c lib/hdcap_demux.c lib/hdcap_control.c lib/hdcap_tables.c lib/hdcap_modes.c lib/hdcap_session.c lib/hdcap_resample.c lib/hdcap_deint.c lib/hdcap_hdcp.c
LIB_OBJ = $(LIB_SRC:.c=.o)

all: libhdcap.a hdcap-capture

libhdcap.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

hdcap-capture: tools/hdcap-capture.o libhdcap.a
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c include/hdcap.h lib/hdcap_tables.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(LIB_OBJ) tools/*.o libhdcap.a hdcap-capture

.PHONY: all clean
