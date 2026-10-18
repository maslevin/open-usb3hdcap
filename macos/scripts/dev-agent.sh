#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# Load/unload hdcapd as a transient per-user LaunchAgent for development (the plist stays in
# macos/build, nothing is installed). Clients in this login session can then reach the service.
#   macos/scripts/dev-agent.sh load | unload | log
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
LABEL=org.open-usb3hdcap.hdcapd
PLIST="$HERE/build/$LABEL.plist"
LOG="$HERE/build/hdcapd.log"
case "$1" in
load)
  cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>$HERE/build/hdcapd</string></array>
  <key>MachServices</key><dict><key>$LABEL</key><true/></dict>
  <key>ProcessType</key><string>Interactive</string>
  <key>StandardErrorPath</key><string>$LOG</string>
</dict></plist>
PL
  launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
  launchctl bootstrap "gui/$(id -u)" "$PLIST"
  echo "loaded $LABEL (on demand; log: $LOG)" ;;
unload)
  launchctl bootout "gui/$(id -u)/$LABEL" && echo "unloaded $LABEL" ;;
log)
  tail -n 40 "$LOG" ;;
*) echo "usage: $0 load|unload|log"; exit 2 ;;
esac
