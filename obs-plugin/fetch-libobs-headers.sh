#!/bin/sh
# Fetch the libobs headers matching the installed OBS.app into third_party/obs-studio-<version>.
# usage: obs-plugin/fetch-libobs-headers.sh [/Applications/OBS.app]
set -e
APP=${1:-/Applications/OBS.app}
VER=$(defaults read "$APP/Contents/Info.plist" CFBundleShortVersionString)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/third_party/obs-studio-$VER"
if [ ! -d "$DEST/libobs" ]; then
  git clone -q --depth 1 --branch "$VER" --filter=blob:none --sparse https://github.com/obsproject/obs-studio.git "$DEST"
  git -C "$DEST" sparse-checkout set libobs
fi
echo "$DEST/libobs"
