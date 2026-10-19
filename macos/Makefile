# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# macOS system drivers: hdcapd daemon, test client, Core Audio HAL plug-in, camera extension + host app.
# Signing: ad-hoc by default. For Developer ID builds copy signing.mk.example to signing.mk and fill
# it in (bundle IDs, team, identities, provisioning profiles, notary profile).
CC      ?= cc
CFLAGS  ?= -O2 -g -Wall -Wextra -std=gnu11
CFLAGS  += -I../include $(shell pkg-config --cflags libusb-1.0)
LIBS     = ../libhdcap.a $(shell pkg-config --libs libusb-1.0)
FW       = -framework CoreFoundation -framework IOSurface
OUT      = build
-include signing.mk
BUNDLE_ID     ?= org.open-usb3hdcap.app
SIGN_IDENTITY ?= -
TEAM_ID       ?= TEAMID
ifeq ($(SIGN_IDENTITY),-)
CODESIGN = codesign --force --sign -
else
CODESIGN = codesign --force --options runtime --timestamp --sign "$(SIGN_IDENTITY)"
endif
SUBST    = sed -e 's/@TEAM_ID@/$(TEAM_ID)/g' -e 's/@BUNDLE_ID@/$(BUNDLE_ID)/g'
# hdcapd ships alone in /Library, so it is universal with libusb linked statically
# (packaging/build-libusb-universal.sh); without that library it is built natively against pkg-config libusb.
LIBUSB_U = ../third_party/libusb-universal
LIB_SRC  = $(wildcard ../lib/*.c)

HAL      = $(OUT)/USB3HDCAP.driver
APP      = $(OUT)/USB3HDCAP.app
EXT      = $(APP)/Contents/Library/SystemExtensions/$(BUNDLE_ID).camera.systemextension
UNIV     = -arch arm64 -arch x86_64

all: $(OUT)/hdcapd $(OUT)/hdcapd-client $(HAL) $(OUT)/hal-harness $(OUT)/frame-source-test $(OUT)/camera-harness $(APP)

../libhdcap.a: FORCE
	$(MAKE) -C .. libhdcap.a

ifneq ($(wildcard $(LIBUSB_U)/lib/libusb-1.0.a),)
$(OUT)/hdcapd: hdcapd/hdcapd.c common/hdcapd_protocol.h $(LIB_SRC) ../include/hdcap.h
	@mkdir -p $(OUT)
	$(CC) -O2 -g -Wall -Wextra -std=gnu11 $(UNIV) -mmacosx-version-min=13.0 -I../include -I$(LIBUSB_U)/include/libusb-1.0 \
		-o $@ $< $(LIB_SRC) $(LIBUSB_U)/lib/libusb-1.0.a $(FW) -framework IOKit -framework Security
	$(CODESIGN) --identifier $(BUNDLE_ID).hdcapd $@
else
$(OUT)/hdcapd: hdcapd/hdcapd.c common/hdcapd_protocol.h ../libhdcap.a
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS) $(FW)
	$(CODESIGN) --identifier $(BUNDLE_ID).hdcapd $@
endif

$(OUT)/hdcapd-client: hdcapd/hdcapd-client.c common/hdcapd_protocol.h ../libhdcap.a
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LIBS) $(FW)

# The HAL plug-in links no libusb: it only talks to hdcapd. Universal so the bundle works on any Mac.
$(HAL): hal/hdcap_hal.c hal/Info.plist common/hdcapd_protocol.h
	@mkdir -p $@/Contents/MacOS
	$(SUBST) hal/Info.plist > $@/Contents/Info.plist
	$(CC) -O2 -g -Wall -Wextra -std=gnu11 -arch arm64 -arch x86_64 -mmacosx-version-min=11.0 -bundle \
		-o $@/Contents/MacOS/USB3HDCAP hal/hdcap_hal.c -framework CoreFoundation -framework CoreAudio
	rm -rf $@/Contents/MacOS/*.dSYM   # one-step -g builds leave debug symbols next to the binary
	$(CODESIGN) $@

$(OUT)/hal-harness: hal/hal-harness.c
	@mkdir -p $(OUT)
	$(CC) -O2 -g -Wall -Wextra -o $@ $< -framework CoreFoundation -framework CoreAudio

CAMFW    = -framework Foundation -framework CoreMedia -framework CoreVideo -framework IOSurface -framework VideoToolbox
$(OUT)/frame-source-test: camera/frame-source-test.m camera/HDCapFrameSource.m camera/HDCapFrameSource.h common/hdcapd_protocol.h ../libhdcap.a
	@mkdir -p $(OUT)
	$(CC) -O2 -g -Wall -Wextra -fobjc-arc -I../include -o $@ camera/frame-source-test.m camera/HDCapFrameSource.m ../libhdcap.a $(CAMFW)

# Universal libhdcap is not needed: the extension and app only speak XPC to hdcapd.
CAM_SRC  = camera/main.m camera/CameraExtension.m camera/HDCapFrameSource.m
$(APP): app/HostApp.m app/Info.plist app/app.entitlements $(CAM_SRC) $(wildcard signing.mk) camera/HDCapFrameSource.h camera/CameraExtension.h camera/Info.plist camera/camera.entitlements common/hdcapd_protocol.h
	rm -rf $@
	@mkdir -p $@/Contents/MacOS $(EXT)/Contents/MacOS
	$(SUBST) camera/Info.plist > $(EXT)/Contents/Info.plist
	$(CC) -O2 -g -Wall -Wextra -fobjc-arc $(UNIV) -mmacosx-version-min=13.0 -o $(EXT)/Contents/MacOS/$(BUNDLE_ID).camera \
		$(CAM_SRC) $(CAMFW) -framework CoreMediaIO -framework CoreAudio
	$(SUBST) app/Info.plist > $@/Contents/Info.plist
	$(CC) -O2 -g -Wall -Wextra -fobjc-arc $(UNIV) -mmacosx-version-min=13.0 -o $@/Contents/MacOS/USB3HDCAP app/HostApp.m \
		-framework Cocoa -framework SystemExtensions
	$(SUBST) camera/camera.entitlements > $(OUT)/camera.entitlements
	$(SUBST) app/app.entitlements > $(OUT)/app.entitlements
	$(if $(PROFILE_EXT),cp "$(PROFILE_EXT)" $(EXT)/Contents/embedded.provisionprofile)
	$(if $(PROFILE_APP),cp "$(PROFILE_APP)" $@/Contents/embedded.provisionprofile)
	rm -rf $@/Contents/MacOS/*.dSYM $(EXT)/Contents/MacOS/*.dSYM
	$(CODESIGN) --entitlements $(OUT)/camera.entitlements $(EXT)
ifeq ($(SIGN_IDENTITY),-)
	@# ad-hoc: restricted entitlements (system-extension.install) would make AMFI refuse to launch the app
	codesign --force --sign - --options runtime $@
else
	$(CODESIGN) --entitlements $(OUT)/app.entitlements $@
endif

$(OUT)/camera-harness: camera/camera-harness.m camera/CameraExtension.m camera/CameraExtension.h camera/HDCapFrameSource.m camera/HDCapFrameSource.h ../libhdcap.a
	@mkdir -p $(OUT)
	$(CC) -O2 -g -Wall -Wextra -fobjc-arc -I../include -o $@ camera/camera-harness.m camera/CameraExtension.m camera/HDCapFrameSource.m \
		../libhdcap.a $(CAMFW) -framework CoreMediaIO -framework CoreAudio

# Notarize and staple the host app (system extensions only activate from notarized apps).
# Needs signing.mk with SIGN_IDENTITY and NOTARY_PROFILE.
notarize-app: $(APP)
	@test "$(SIGN_IDENTITY)" != "-" -a -n "$(NOTARY_PROFILE)" || { echo "configure signing.mk first"; exit 1; }
	rm -f $(OUT)/app.zip && ditto -c -k --keepParent $(APP) $(OUT)/app.zip
	xcrun notarytool submit $(OUT)/app.zip --keychain-profile "$(NOTARY_PROFILE)" --wait
	xcrun stapler staple $(APP)
	spctl -a -vv -t exec $(APP)

# Signed (and, with NOTARY_PROFILE, notarized) installer: app + hdcapd LaunchDaemon + HAL plug-in.
VERSION ?= $(shell /usr/libexec/PlistBuddy -c 'Print CFBundleShortVersionString' app/Info.plist)
pkg: notarize-app $(OUT)/hdcapd $(HAL)
	OUT=$(OUT) BUNDLE_ID=$(BUNDLE_ID) VERSION=$(VERSION) INSTALLER_IDENTITY="$(INSTALLER_IDENTITY)" \
		NOTARY_PROFILE="$(NOTARY_PROFILE)" scripts/build-pkg.sh

clean:
	rm -rf $(OUT)

FORCE:
.PHONY: all clean FORCE notarize-app pkg
