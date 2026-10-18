# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# macOS system drivers: hdcapd daemon, test client, Core Audio HAL plug-in, camera extension + host app.
# Signing: ad-hoc by default. With a Developer ID:  make SIGN_IDENTITY="Developer ID Application: ..." TEAM_ID=XXXXXXXXXX
# (the camera extension also needs a provisioning profile: PROFILE_APP=... PROFILE_EXT=... .provisionprofile files).
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
CFLAGS  += -I../include $(shell pkg-config --cflags libusb-1.0)
LIBS     = ../libhdcap.a $(shell pkg-config --libs libusb-1.0)
FW       = -framework CoreFoundation -framework IOSurface
OUT      = build

HAL      = $(OUT)/USB3HDCAP.driver
APP      = $(OUT)/USB3HDCAP.app
EXT      = $(APP)/Contents/Library/SystemExtensions/org.open-usb3hdcap.app.camera.systemextension
SIGN_IDENTITY ?= -
TEAM_ID  ?= TEAMID
UNIV     = -arch arm64 -arch x86_64

all: $(OUT)/hdcapd $(OUT)/hdcapd-client $(HAL) $(OUT)/hal-harness $(OUT)/frame-source-test $(OUT)/camera-harness $(APP)

../libhdcap.a: FORCE
	$(MAKE) -C .. libhdcap.a

$(OUT)/hdcapd: hdcapd/hdcapd.c common/hdcapd_protocol.h ../libhdcap.a
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS) $(FW)
	codesign --force --sign - --identifier org.open-usb3hdcap.hdcapd $@

$(OUT)/hdcapd-client: hdcapd/hdcapd-client.c common/hdcapd_protocol.h ../libhdcap.a
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS) $(FW)

# The HAL plug-in links no libusb: it only talks to hdcapd. Universal so the bundle works on any Mac.
$(HAL): hal/hdcap_hal.c hal/Info.plist common/hdcapd_protocol.h
	@mkdir -p $@/Contents/MacOS
	cp hal/Info.plist $@/Contents/Info.plist
	$(CC) -O2 -g -Wall -Wextra -std=gnu11 -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -bundle \
		-o $@/Contents/MacOS/USB3HDCAP hal/hdcap_hal.c -framework CoreFoundation -framework CoreAudio
	codesign --force --sign - $@

$(OUT)/hal-harness: hal/hal-harness.c
	@mkdir -p $(OUT)
	$(CC) -O2 -g -Wall -Wextra -o $@ $< -framework CoreFoundation -framework CoreAudio

CAMFW    = -framework Foundation -framework CoreMedia -framework CoreVideo -framework IOSurface -framework VideoToolbox
$(OUT)/frame-source-test: camera/frame-source-test.m camera/HDCapFrameSource.m camera/HDCapFrameSource.h common/hdcapd_protocol.h ../libhdcap.a
	@mkdir -p $(OUT)
	$(CC) -O2 -g -Wall -Wextra -fobjc-arc -I../include -o $@ camera/frame-source-test.m camera/HDCapFrameSource.m ../libhdcap.a $(CAMFW)

# Universal libhdcap is not needed: the extension and app only speak XPC to hdcapd.
CAM_SRC  = camera/main.m camera/CameraExtension.m camera/HDCapFrameSource.m
$(APP): app/HostApp.m app/Info.plist app/app.entitlements $(CAM_SRC) camera/HDCapFrameSource.h camera/CameraExtension.h camera/Info.plist camera/camera.entitlements common/hdcapd_protocol.h
	rm -rf $@
	@mkdir -p $@/Contents/MacOS $(EXT)/Contents/MacOS
	sed 's/@TEAM_ID@/$(TEAM_ID)/g' camera/Info.plist > $(EXT)/Contents/Info.plist
	$(CC) -O2 -g -Wall -Wextra -fobjc-arc $(UNIV) -mmacosx-version-min=13.0 -o $(EXT)/Contents/MacOS/org.open-usb3hdcap.app.camera \
		$(CAM_SRC) $(CAMFW) -framework CoreMediaIO -framework CoreAudio
	cp app/Info.plist $@/Contents/Info.plist
	$(CC) -O2 -g -Wall -Wextra -fobjc-arc $(UNIV) -mmacosx-version-min=13.0 -o $@/Contents/MacOS/USB3HDCAP app/HostApp.m \
		-framework Cocoa -framework SystemExtensions
	sed 's/@TEAM_ID@/$(TEAM_ID)/g' camera/camera.entitlements > $(OUT)/camera.entitlements
	sed 's/@TEAM_ID@/$(TEAM_ID)/g' app/app.entitlements > $(OUT)/app.entitlements
	$(if $(PROFILE_EXT),cp "$(PROFILE_EXT)" $(EXT)/Contents/embedded.provisionprofile)
	$(if $(PROFILE_APP),cp "$(PROFILE_APP)" $@/Contents/embedded.provisionprofile)
	codesign --force --sign "$(SIGN_IDENTITY)" --options runtime --entitlements $(OUT)/camera.entitlements $(EXT)
ifeq ($(SIGN_IDENTITY),-)
	@# ad-hoc: restricted entitlements (system-extension.install) would make AMFI refuse to launch the app
	codesign --force --sign - --options runtime $@
else
	codesign --force --sign "$(SIGN_IDENTITY)" --options runtime --timestamp --entitlements $(OUT)/app.entitlements $@
endif

$(OUT)/camera-harness: camera/camera-harness.m camera/CameraExtension.m camera/CameraExtension.h camera/HDCapFrameSource.m camera/HDCapFrameSource.h ../libhdcap.a
	@mkdir -p $(OUT)
	$(CC) -O2 -g -Wall -Wextra -fobjc-arc -I../include -o $@ camera/camera-harness.m camera/CameraExtension.m camera/HDCapFrameSource.m \
		../libhdcap.a $(CAMFW) -framework CoreMediaIO -framework CoreAudio

clean:
	rm -rf $(OUT)

FORCE:
.PHONY: all clean FORCE
