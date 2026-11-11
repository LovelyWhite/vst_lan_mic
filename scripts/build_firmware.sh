#!/usr/bin/env bash
# Build the ESP32-S3 firmware with PlatformIO and publish the flashable bins to site/firmware,
# which is what the web flasher (config.html -> firmware/manifest.json) serves.
# manifest.json's "version" is synced from common/Version.h, the single source of truth,
# so config.html's update check never compares against a stale number.
#
# Usage:
#   ./build_firmware.sh

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
PROJ="$SCRIPT_DIR/../esp32s3"
ENV_NAME="esp32-s3-devkitc-1"
BUILD="$PROJ/.pio/build/$ENV_NAME"
DEST="$SCRIPT_DIR/../site/firmware"
VER_H="$SCRIPT_DIR/../common/Version.h"

pio run -d "$PROJ" -e "$ENV_NAME"

for f in firmware.bin bootloader.bin partitions.bin; do
  [ -f "$BUILD/$f" ] || { echo "错误: 构建产物缺失 $BUILD/$f" >&2; exit 1; }
  cp -f "$BUILD/$f" "$DEST/$f"
done

# boot_app0.bin ships inside the framework package instead of the build dir
PKG_DIR="${PLATFORMIO_CORE_DIR:-$HOME/.platformio}/packages"
shopt -s nullglob
APP0=("$PKG_DIR"/framework-arduinoespressif32*/tools/partitions/boot_app0.bin)
shopt -u nullglob
if [ "${#APP0[@]}" -gt 0 ]; then
  cp -f "${APP0[0]}" "$DEST/boot_app0.bin"
else
  echo "警告: 未在 $PKG_DIR 找到 boot_app0.bin，沿用 site/firmware 中的旧文件" >&2
fi

# The device reports "vMAJOR.MINOR.PATCH" from Version.h, so keep the same shape here.
VERSION=$(sed -n 's/^#define LANMIC_VERSION "\(.*\)".*/\1/p' "$VER_H" | head -n1)
[ -n "$VERSION" ] || { echo "错误: 未能从 $VER_H 解析出版本" >&2; exit 1; }
case "$VERSION" in v*) ;; *) VERSION="v$VERSION" ;; esac

MANIFEST="$DEST/manifest.json"
sed "s/^\([[:space:]]*\"version\"[[:space:]]*:[[:space:]]*\)\"[^\"]*\"/\1\"$VERSION\"/" \
  "$MANIFEST" > "$MANIFEST.tmp"
grep -q "\"version\":[[:space:]]*\"$VERSION\"" "$MANIFEST.tmp" \
  || { echo "错误: $MANIFEST 中没有 version 字段，未改动" >&2; rm -f "$MANIFEST.tmp"; exit 1; }
mv -f "$MANIFEST.tmp" "$MANIFEST"
echo "manifest.json version -> $VERSION"

echo "已复制到 $DEST:"
ls -l "$DEST"/*.bin
