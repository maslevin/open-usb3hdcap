# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# macOS system drivers: hdcapd daemon, test client (HAL plug-in and camera extension: see their dirs).
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
CFLAGS  += -I../include $(shell pkg-config --cflags libusb-1.0)
LIBS     = ../libhdcap.a $(shell pkg-config --libs libusb-1.0)
FW       = -framework CoreFoundation -framework IOSurface
OUT      = build

all: $(OUT)/hdcapd $(OUT)/hdcapd-client

../libhdcap.a: FORCE
	$(MAKE) -C .. libhdcap.a

$(OUT)/hdcapd: hdcapd/hdcapd.c common/hdcapd_protocol.h ../libhdcap.a
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS) $(FW)
	codesign --force --sign - --identifier org.open-usb3hdcap.hdcapd $@

$(OUT)/hdcapd-client: hdcapd/hdcapd-client.c common/hdcapd_protocol.h ../libhdcap.a
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS) $(FW)

clean:
	rm -rf $(OUT)

FORCE:
.PHONY: all clean FORCE
