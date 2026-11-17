#!/usr/bin/env bash
# Builds the firmware with PlatformIO and merges the bootloader, partition table,
# boot_app0 and the app into ONE image that is flashed at address 0x0.
# Used by the GitHub workflow (web installer and releases).
#
# It also writes, next to OUTPUT.bin, what firmware UPDATES need (they keep the user's settings and files):
#   firmware-app.bin  the app image only. The stick installs it over Wi-Fi (web page, or by itself from GitHub
#                     Pages) and the web installer's "Update" button flashes it at 0x10000
#   otadata.bin       the OTA bookkeeping area (0xE000): makes the bootloader start the app at 0x10000, which
#                     matters when an earlier Wi-Fi update had put the running firmware in the other slot
#   latest.json       {"version","file","size","md5"}: what the stick reads to learn that an update exists
#
# Usage: bash tools/build-merged.sh OUTPUT.bin
set -euo pipefail

OUT="${1:?usage: build-merged.sh OUTPUT.bin}"
BUILD=.pio/build/t-dongle-s3

pio run

BOOT_APP0=$(find ~/.platformio -name boot_app0.bin | head -n 1)
if [ -z "$BOOT_APP0" ]; then echo "boot_app0.bin not found"; exit 1; fi

mkdir -p "$(dirname "$OUT")"
esptool.py --chip esp32s3 merge_bin \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  -o "$OUT" \
  0x0     "$BUILD/bootloader.bin" \
  0x8000  "$BUILD/partitions.bin" \
  0xe000  "$BOOT_APP0" \
  0x10000 "$BUILD/firmware.bin"
ls -la "$OUT"

# The web installer writes the user's settings into the "cfg" area (0xD000-0xDFFF),
# so it must be blank in the published image.
python3 - "$OUT" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
assert len(d) > 0x10000, "merged image is too small"
assert set(d[0xD000:0xE000]) == {0xFF}, "cfg area is not blank - check partitions.csv"
print("cfg area is blank, OK")
PY

# ---- update files (see the top of this script) ----
DIR="$(dirname "$OUT")"
VER=$(grep -oP '#define FW_VERSION "\K[^"]+' src/main.cpp)
cp "$BUILD/firmware.bin" "$DIR/firmware-app.bin"
cp "$BOOT_APP0" "$DIR/otadata.bin"
SIZE=$(stat -c %s "$DIR/firmware-app.bin")
MD5=$(md5sum "$DIR/firmware-app.bin" | cut -d' ' -f1)
printf '{"version":"%s","file":"firmware-app.bin","size":%s,"md5":"%s"}\n' "$VER" "$SIZE" "$MD5" > "$DIR/latest.json"
python3 - "$DIR/firmware-app.bin" <<'PY'
import sys
d = open(sys.argv[1], "rb").read()
# an ESP32-S3 app image: magic 0xE9, chip id 9, and the app descriptor right after the first segment header
assert d[0] == 0xE9 and d[12] == 0x09 and d[13] == 0x00, "firmware-app.bin is not an ESP32-S3 image"
assert int.from_bytes(d[32:36], "little") == 0xABCD5432, "firmware-app.bin has no app descriptor"
assert len(d) < 0x640000, "firmware-app.bin does not fit an app slot"
print("firmware-app.bin looks like an app image, OK")
PY
cat "$DIR/latest.json"
