#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# Install/uninstall the system-wide pieces (run with sudo):
#   hdcapd       -> /Library/Application Support/open-usb3hdcap/hdcapd  (LaunchDaemon, system domain)
#   HAL plug-in  -> /Library/Audio/Plug-Ins/HAL/USB3HDCAP.driver        (coreaudiod is restarted)
# coreaudiod and the camera extension can only reach Mach services in the system domain, so the
# per-user dev agent (scripts/dev-agent.sh) is not enough for them; unload it first to avoid two
# daemons competing for the device.
#   sudo macos/scripts/install-system.sh install | uninstall
set -e
HERE=$(cd "$(dirname "$0")/.." && pwd)
LABEL=org.open-usb3hdcap.hdcapd
DEST="/Library/Application Support/open-usb3hdcap"
PLIST="/Library/LaunchDaemons/$LABEL.plist"
HAL="/Library/Audio/Plug-Ins/HAL/USB3HDCAP.driver"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
case "$1" in
install)
  [ -x "$HERE/build/hdcapd" ] && [ -d "$HERE/build/USB3HDCAP.driver" ] || { echo "build first: make -C macos"; exit 1; }
  launchctl bootout "system/$LABEL" 2>/dev/null || true
  mkdir -p "$DEST"
  install -m 755 "$HERE/build/hdcapd" "$DEST/hdcapd"
  cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>$DEST/hdcapd</string></array>
  <key>MachServices</key><dict><key>$LABEL</key><true/></dict>
  <key>ProcessType</key><string>Interactive</string>
  <key>StandardErrorPath</key><string>/Library/Logs/hdcapd.log</string>
</dict></plist>
PL
  chown root:wheel "$PLIST"; chmod 644 "$PLIST"
  launchctl bootstrap system "$PLIST"
  rm -rf "$HAL"
  cp -R "$HERE/build/USB3HDCAP.driver" "$HAL"
  chown -R root:wheel "$HAL"
  killall coreaudiod 2>/dev/null || true
  echo "installed; 'USB3HDCAP' should now appear as an audio input" ;;
uninstall)
  launchctl bootout "system/$LABEL" 2>/dev/null || true
  rm -f "$PLIST"; rm -rf "$DEST" "$HAL"
  killall coreaudiod 2>/dev/null || true
  echo "uninstalled" ;;
*) echo "usage: sudo $0 install|uninstall"; exit 2 ;;
esac
