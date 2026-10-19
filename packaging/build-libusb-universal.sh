#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# Build universal (arm64 + x86_64) libusb: a dylib bundled into the OBS plugin, and a static library
# linked into hdcapd (a bare daemon binary cannot carry its own Frameworks directory).
# Output: third_party/libusb-universal/{lib/libusb-1.0.0.dylib,lib/libusb-1.0.a,include/libusb-1.0/libusb.h}
set -e
VER=1.0.30
SHA=fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf
MINOS=${MACOSX_DEPLOYMENT_TARGET:-13.0}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK="$ROOT/third_party/libusb-build"
OUT="$ROOT/third_party/libusb-universal"
mkdir -p "$WORK" && cd "$WORK"
if [ ! -f "libusb-$VER.tar.bz2" ]; then
  curl -sSL -o "libusb-$VER.tar.bz2" "https://github.com/libusb/libusb/releases/download/v$VER/libusb-$VER.tar.bz2"
fi
echo "$SHA  libusb-$VER.tar.bz2" | shasum -a 256 -c -
for ARCH in arm64 x86_64; do
  rm -rf "src-$ARCH" && mkdir "src-$ARCH" && tar -xjf "libusb-$VER.tar.bz2" -C "src-$ARCH" --strip-components 1
  HOST=$([ $ARCH = arm64 ] && echo aarch64-apple-darwin || echo x86_64-apple-darwin)
  (cd "src-$ARCH" &&
   CFLAGS="-arch $ARCH -O2 -mmacosx-version-min=$MINOS" LDFLAGS="-arch $ARCH -mmacosx-version-min=$MINOS" \
   ./configure --host=$HOST --prefix="$WORK/inst-$ARCH" --enable-static --enable-shared --disable-examples-build --disable-tests-build >/dev/null &&
   make -j8 >/dev/null && make install >/dev/null)
done
rm -rf "$OUT" && mkdir -p "$OUT/lib" "$OUT/include"
lipo -create "$WORK/inst-arm64/lib/libusb-1.0.0.dylib" "$WORK/inst-x86_64/lib/libusb-1.0.0.dylib" -output "$OUT/lib/libusb-1.0.0.dylib"
install_name_tool -id @rpath/libusb-1.0.0.dylib "$OUT/lib/libusb-1.0.0.dylib"
lipo -create "$WORK/inst-arm64/lib/libusb-1.0.a" "$WORK/inst-x86_64/lib/libusb-1.0.a" -output "$OUT/lib/libusb-1.0.a"
cp -R "$WORK/inst-x86_64/include/libusb-1.0" "$OUT/include/"
cp "src-x86_64/COPYING" "$OUT/COPYING.libusb"
lipo -info "$OUT/lib/libusb-1.0.0.dylib" "$OUT/lib/libusb-1.0.a"
