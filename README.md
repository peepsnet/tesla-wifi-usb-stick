# Wi-Fi USB Stick for the LilyGo T-Dongle S3

Turns a **LilyGo T-Dongle S3 (with the LCD screen)** into a USB flash drive that you can
manage over Wi-Fi.

## Features
- Looks like a normal USB drive (backed by the microSD card) to a PC, car, printer, etc.
- Web page (over the dongle's own Wi-Fi) to upload, download, rename, delete files and make folders
- Can also join your home Wi-Fi (scan, enter a password, or type a hidden network name)
- Screen shows the mode, the IP address, connected clients or free space
- "Make Lock Chime" button on `.wav` files, and the BOOT button picks a random `.wav` (under 1 MB)
  and copies it to `/LockChime.wav`
- Drive is named "Boombox"

## Install (no tools needed)
Open the installer page (GitHub Pages for this repository) in **Chrome or Edge on a desktop**
and follow the steps. You must hold the dongle's button while plugging it in so it enters
flashing mode.

## Wi-Fi name and password
The dongle creates its own Wi-Fi network. The name and password are the `AP_SSID` and `AP_PASS`
lines near the top of `src/main.cpp`. **Change them before you publish or flash**, because
anyone who can read this repository can read them.

## Important notes
- Only one side can use the card at a time. In **USB mode** the host owns the card; in
  **Wi-Fi mode** the dongle owns it and the host sees the drive as ejected.
- The web page has no login. Anyone who can join the dongle's Wi-Fi (or your home Wi-Fi when the
  dongle is connected to it) can change the files.
- Use a **FAT32** card. exFAT cards work as a drive but show "n/a" for free space and aren't renamed.

## Building it yourself
1. Install VS Code + the PlatformIO extension.
2. Open this folder and run **PlatformIO: Build** (or `pio run`).
3. Hold the dongle's button while plugging it in, then **PlatformIO: Upload**.

## How the web installer is built
`.github/workflows/build-and-deploy.yml` builds the firmware on GitHub every time you push to
`main`, merges the pieces into one `firmware.bin`, and publishes it with `site/index.html`
(using [ESP Web Tools](https://esphome.github.io/esp-web-tools/)) to GitHub Pages.

If a flashed dongle doesn't boot, try changing `--flash_mode dio` to `--flash_mode qio` in that
workflow file.
