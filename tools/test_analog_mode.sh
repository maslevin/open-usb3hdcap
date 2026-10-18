#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# Capture from VGA/Component with auto mode detection, save a PNG and report timing + picture bounds.
# usage: tools/test_analog_mode.sh <vga|component> <label> [extra hdcap-capture args]
set -e
IN=$1; LABEL=$2; shift 2
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${TMPDIR:-/tmp}/hdcap_modes/$LABEL
rm -rf "$OUT"; mkdir -p "$OUT"
"$ROOT/hdcap-capture" -i "$IN" -t 2 -n 4 -o "$OUT" "$@" 2>&1 | grep -E '^USB|^signal|^mode|2.0s|no mode'
F=$(ls "$OUT"/frame_003.ppm 2>/dev/null || true)
if [ -n "$F" ]; then
  sips -s format png "$F" --out "$OUT/frame.png" >/dev/null
  "$ROOT/../tools/.venv/bin/python" -I "$ROOT/../tools/active_area.py" "$F" | head -1
  echo "png: $OUT/frame.png"
fi
