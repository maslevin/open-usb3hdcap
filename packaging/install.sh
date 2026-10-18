#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# Install the USB3HDCAP OBS plugin for the current user (run from the unzipped folder).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST="$HOME/Library/Application Support/obs-studio/plugins"
if pgrep -x OBS >/dev/null; then echo "Please quit OBS first."; exit 1; fi
mkdir -p "$DEST"
rm -rf "$DEST/usb3hdcap.plugin"
ditto "$HERE/usb3hdcap.plugin" "$DEST/usb3hdcap.plugin"
# Files downloaded from the internet are quarantined; an un-notarized plugin would be blocked.
/usr/bin/xattr -dr com.apple.quarantine "$DEST/usb3hdcap.plugin" 2>/dev/null || true   # Apple's xattr; a pip-installed one can shadow it in PATH
echo "Installed to $DEST/usb3hdcap.plugin"
echo "Start OBS and add a source: + > USB3HDCAP Capture."
