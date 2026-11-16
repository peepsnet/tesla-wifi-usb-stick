#!/usr/bin/env bash
# Builds the firmware with PlatformIO and merges the bootloader, partition table,
# boot_app0 and the app into ONE image that is flashed at address 0x0.
# Used by the GitHub workflow (web installer and releases).
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
