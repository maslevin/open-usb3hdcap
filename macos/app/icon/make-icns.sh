#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# Render AppIcon.svg to a macOS .icns: render-svg.m (AppKit) rasterizes the SVG onto a transparent
# canvas at every iconset size, iconutil packs them.   make-icns.sh AppIcon.svg OUT.icns
# (Quick Look thumbnails are composited on white, which left white corners around the tile.)
set -e
SVG="$1"; OUT="$2"
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
clang -fobjc-arc -O2 -o "$WORK/render-svg" "$HERE/render-svg.m" -framework AppKit
SET="$WORK/AppIcon.iconset"
mkdir "$SET"
for s in 16 32 128 256 512; do
  "$WORK/render-svg" "$SVG" "$SET/icon_${s}x${s}.png" $s
  "$WORK/render-svg" "$SVG" "$SET/icon_${s}x${s}@2x.png" $((s * 2))
done
iconutil -c icns "$SET" -o "$OUT"
