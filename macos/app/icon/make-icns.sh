#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# Render AppIcon.svg to a macOS .icns: Quick Look (WebKit) renders the SVG at 1024 px, sips scales it
# to every iconset size, iconutil packs them.   make-icns.sh AppIcon.svg OUT.icns
set -e
SVG="$1"; OUT="$2"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
qlmanage -t -s 1024 -o "$WORK" "$SVG" >/dev/null 2>&1
PNG="$WORK/$(basename "$SVG").png"
[ -f "$PNG" ] || { echo "qlmanage did not render $SVG" >&2; exit 1; }
SET="$WORK/AppIcon.iconset"
mkdir "$SET"
for s in 16 32 128 256 512; do
  sips -z $s $s "$PNG" --out "$SET/icon_${s}x${s}.png" >/dev/null
  d=$((s * 2))
  sips -z $d $d "$PNG" --out "$SET/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$SET" -o "$OUT"
