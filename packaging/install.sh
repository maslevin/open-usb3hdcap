#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# Install the USB3HDCAP OBS plugin for the current user (run from the unzipped folder).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST="$HOME/Library/Application Support/obs-studio/plugins"
if pgrep -x OBS >/dev/null; then echo "Please quit OBS first."; exit 1; fi
# The notice (no warranty, limitation of liability) must be accepted, as in the macOS installer.
if [ -f "$HERE/NOTICE" ] && [ "$1" != "--accept" ]; then
  ${PAGER:-more} "$HERE/NOTICE"
  printf "Do you accept the terms of this notice and the GPL licence? [y/N] "
  read -r answer
  case "$answer" in y|Y|yes|YES) ;; *) echo "Not installed."; exit 1 ;; esac
fi
mkdir -p "$DEST"
rm -rf "$DEST/usb3hdcap.plugin"
ditto "$HERE/usb3hdcap.plugin" "$DEST/usb3hdcap.plugin"
# Files downloaded from the internet are quarantined; an un-notarized plugin would be blocked.
/usr/bin/xattr -dr com.apple.quarantine "$DEST/usb3hdcap.plugin" 2>/dev/null || true   # Apple's xattr; a pip-installed one can shadow it in PATH
echo "Installed to $DEST/usb3hdcap.plugin"
echo "Start OBS and add a source: + > USB3HDCAP Capture."
