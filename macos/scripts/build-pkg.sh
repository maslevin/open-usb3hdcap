#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Matt Slevinsky
# Build the macOS installer package: USB3HDCAP.app (camera extension host) in /Applications, hdcapd
# as a LaunchDaemon, and the Core Audio HAL plug-in. Signed with the Developer ID Installer identity,
# notarized and stapled when NOTARY_PROFILE is set. Run via `make -C macos pkg` (reads signing.mk).
# Environment: OUT BUNDLE_ID VERSION INSTALLER_IDENTITY [NOTARY_PROFILE]
set -e
: "${OUT:?}" "${BUNDLE_ID:?}" "${VERSION:?}" "${INSTALLER_IDENTITY:?set INSTALLER_IDENTITY in signing.mk}"
LABEL=org.open-usb3hdcap.hdcapd
SUPPORT="Library/Application Support/open-usb3hdcap"
ROOT="$OUT/pkgroot"
SCRIPTS="$OUT/pkgscripts"
PKG="$OUT/USB3HDCAP-$VERSION.pkg"

rm -rf "$ROOT" "$SCRIPTS" "$OUT/component.pkg" "$OUT/components.plist" "$PKG"
mkdir -p "$ROOT/Applications" "$ROOT/$SUPPORT" "$ROOT/Library/LaunchDaemons" "$ROOT/Library/Audio/Plug-Ins/HAL" "$SCRIPTS"
ditto "$OUT/USB3HDCAP.app" "$ROOT/Applications/USB3HDCAP.app"
ditto "$OUT/USB3HDCAP.driver" "$ROOT/Library/Audio/Plug-Ins/HAL/USB3HDCAP.driver"
install -m 755 "$OUT/hdcapd" "$ROOT/$SUPPORT/hdcapd"

cat > "$ROOT/Library/LaunchDaemons/$LABEL.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>/$SUPPORT/hdcapd</string></array>
  <key>MachServices</key><dict><key>$LABEL</key><true/></dict>
  <key>ProcessType</key><string>Interactive</string>
  <key>StandardErrorPath</key><string>/Library/Logs/hdcapd.log</string>
</dict></plist>
PL
chmod 644 "$ROOT/Library/LaunchDaemons/$LABEL.plist"

cat > "$ROOT/$SUPPORT/uninstall.sh" <<UN
#!/bin/sh
# Remove USB3HDCAP. First open USB3HDCAP.app and click "Remove Camera" (macOS only lets the app
# deactivate its camera extension). Then: sudo "/$SUPPORT/uninstall.sh"
[ "\$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
launchctl bootout system/$LABEL 2>/dev/null || true
rm -f /Library/LaunchDaemons/$LABEL.plist
rm -rf /Library/Audio/Plug-Ins/HAL/USB3HDCAP.driver /Applications/USB3HDCAP.app "/$SUPPORT"
killall coreaudiod 2>/dev/null || true
pkgutil --forget $BUNDLE_ID.pkg >/dev/null 2>&1 || true
echo "USB3HDCAP removed"
UN
chmod 755 "$ROOT/$SUPPORT/uninstall.sh"

cat > "$SCRIPTS/preinstall" <<'PRE'
#!/bin/sh
# stop a running daemon so its binary can be replaced
launchctl bootout system/org.open-usb3hdcap.hdcapd 2>/dev/null || true
exit 0
PRE
cat > "$SCRIPTS/postinstall" <<'POST'
#!/bin/sh
PL=/Library/LaunchDaemons/org.open-usb3hdcap.hdcapd.plist
chown root:wheel "$PL"; chmod 644 "$PL"
chown -R root:wheel /Library/Audio/Plug-Ins/HAL/USB3HDCAP.driver
launchctl bootstrap system "$PL" 2>/dev/null || launchctl kickstart -k system/org.open-usb3hdcap.hdcapd 2>/dev/null || true
killall coreaudiod 2>/dev/null || true   # load the HAL plug-in
exit 0
POST
chmod 755 "$SCRIPTS/preinstall" "$SCRIPTS/postinstall"

# Install bundles exactly where the payload puts them (no relocation to copies elsewhere on disk).
pkgbuild --analyze --root "$ROOT" "$OUT/components.plist" >/dev/null
i=0; while /usr/libexec/PlistBuddy -c "Print :$i" "$OUT/components.plist" >/dev/null 2>&1; do
  /usr/libexec/PlistBuddy -c "Set :$i:BundleIsRelocatable false" "$OUT/components.plist" 2>/dev/null ||
    /usr/libexec/PlistBuddy -c "Add :$i:BundleIsRelocatable bool false" "$OUT/components.plist"
  i=$((i + 1))
done
pkgbuild --root "$ROOT" --component-plist "$OUT/components.plist" --scripts "$SCRIPTS" \
  --identifier "$BUNDLE_ID.pkg" --version "$VERSION" --install-location / "$OUT/component.pkg" >/dev/null
productbuild --package "$OUT/component.pkg" --sign "$INSTALLER_IDENTITY" --timestamp "$PKG" >/dev/null
pkgutil --check-signature "$PKG" | sed -n '1,4p'

if [ -n "$NOTARY_PROFILE" ]; then
  xcrun notarytool submit "$PKG" --keychain-profile "$NOTARY_PROFILE" --wait
  xcrun stapler staple "$PKG"
  spctl -a -vv -t install "$PKG"
fi
echo "$PKG"
