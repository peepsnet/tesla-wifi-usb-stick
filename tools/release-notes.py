"""Prints the GitHub release notes for one version: its CHANGELOG.md section
followed by flashing instructions.

Usage: python3 tools/release-notes.py VERSION ASSET_NAME APP_ASSET_NAME
"""
import re
import sys

ver, asset, app_asset = sys.argv[1], sys.argv[2], sys.argv[3]
text = open("CHANGELOG.md", encoding="utf-8").read()

# The section that starts with "## <version>" (for example "## 0.6 (2026-11-20)").
m = re.search(r"^## " + re.escape(ver) + r"\b.*?$(.*?)(?=^## |\Z)", text, re.M | re.S)
if not m:
    # "::error::" makes GitHub show the message on the run's summary page.
    print(f"::error title=Release notes::CHANGELOG.md has no '## {ver}' section. "
          f"Add one (for example '## {ver} (date)') before releasing.", file=sys.stderr)
    sys.exit(1)
section = m.group(1).strip()

print(f"""{section}

## Install

**Easiest (lets you choose your own Wi-Fi name and password):** open the
[web installer](https://peepsnet.github.io/tesla-wifi-usb-stick/) in Chrome or Edge on a computer.

**With the file attached below (`{asset}`):** it contains everything (bootloader, partition table, app) and
is flashed at address **0x0**. It uses the default settings listed in the README.

1. Hold the button on the T-Dongle S3 while plugging it in (download mode).
2. Open [esptool-js](https://espressif.github.io/esptool-js/) in Chrome or Edge, click **Connect** and pick the
   dongle.
3. Set the flash address to `0x0`, choose `{asset}`, and click **Program**.
4. Unplug and re-plug the dongle without holding the button.

Command line alternative: `esptool.py --chip esp32s3 write_flash 0x0 {asset}`

## Update (keeps your settings, saved Wi-Fi networks and card)

**Sticks running 0.13 or newer** can update over Wi-Fi: open the stick's web page and use **Firmware update**
(the stick also shows an arrow icon on its screen and on the page when an update exists). If the stick has no
internet, pick the attached `{app_asset}` in that panel.

**Older sticks** (or to update over USB): open the web installer in Chrome or Edge and use **Update**; it flashes
only the app and leaves everything else alone.

Flashing replaces the firmware and does not touch the microSD card. But note **First Run**: a card that is not
set up for the stick yet is erased the first time the stick starts with it (see the README).
""")
