#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# Build, sign and zip the OBS plugin for distribution (universal arm64 + x86_64, bundled libusb).
#
#   packaging/package-macos.sh                 # ad-hoc signed zip in dist/
#   SIGN_IDENTITY="Developer ID Application: Name (TEAMID)" NOTARY_PROFILE=myprofile packaging/package-macos.sh
#       Developer ID signing (hardened runtime) and notarization; NOTARY_PROFILE is a keychain
#       profile created with `xcrun notarytool store-credentials`.
# Env: OBS_HEADERS_VERSION (default 30.0.0, oldest supported OBS), USB3HDCAP_VERSION (default from git).
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
OBSV=${OBS_HEADERS_VERSION:-30.0.0}
VER=${USB3HDCAP_VERSION:-0.3.0-$(git rev-parse --short HEAD)}
IDENT=${SIGN_IDENTITY:--}

[ -f third_party/libusb-universal/lib/libusb-1.0.0.dylib ] || packaging/build-libusb-universal.sh
DEST="third_party/obs-studio-$OBSV"
if [ ! -d "$DEST/libobs" ]; then
  git clone -q --depth 1 --branch "$OBSV" --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git "$DEST"
  git -C "$DEST" sparse-checkout set libobs
fi

rm -rf build-dist
cmake -S obs-plugin -B build-dist -DUSB3HDCAP_DIST=ON -DCMAKE_BUILD_TYPE=Release -DUSB3HDCAP_VERSION="$VER" \
      -DLIBOBS_INCLUDE_DIR="$ROOT/$DEST/libobs" -DLIBUSB_BUNDLE_DIR="$ROOT/third_party/libusb-universal" >/dev/null
cmake --build build-dist >/dev/null
P=build-dist/usb3hdcap.plugin

# Sign inside-out: bundled dylib, then the bundle. Hardened runtime only with a real identity.
OPTS="--force --timestamp=none"
[ "$IDENT" != "-" ] && OPTS="--force --timestamp --options runtime"
codesign $OPTS --sign "$IDENT" "$P/Contents/Frameworks/libusb-1.0.0.dylib"
codesign $OPTS --sign "$IDENT" "$P"
codesign --verify --strict --verbose=1 "$P"

if [ -n "$NOTARY_PROFILE" ] && [ "$IDENT" != "-" ]; then
  ditto -c -k --keepParent "$P" build-dist/notarize.zip
  xcrun notarytool submit build-dist/notarize.zip --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$P"
fi

NAME="usb3hdcap-obs-$VER-macos-universal"
rm -rf "dist/$NAME" "dist/$NAME.zip" && mkdir -p "dist/$NAME"
ditto "$P" "dist/$NAME/usb3hdcap.plugin"
cp packaging/install.sh "dist/$NAME/install.sh"
cp packaging/INSTALL.md "dist/$NAME/INSTALL.md"
cp LICENSE "dist/$NAME/LICENSE"
cp NOTICE "dist/$NAME/NOTICE"
(cd dist && ditto -c -k --keepParent "$NAME" "$NAME.zip")
echo "dist/$NAME.zip  (signed: $([ "$IDENT" = "-" ] && echo ad-hoc || echo "$IDENT"), headers: OBS $OBSV)"
