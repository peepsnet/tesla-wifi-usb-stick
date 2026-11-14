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

## Settings you can choose before flashing
The installer page has a form. Whatever you pick is written into a small "cfg" area of the
dongle's flash together with the firmware:

| Setting | What it does |
|---|---|
| Wi-Fi name / password | The dongle's own Wi-Fi network (password 8-63 characters) |
| Flip screen? | No = normal, Yes = rotate the screen 180 degrees |
| Drive name | The name shown for the drive on your computer (max 11 characters) |
| Start mode | Start as a USB drive, or in Wi-Fi edit mode |
| Largest random lock chime (KB) | The BOOT button only picks `.wav` files smaller than this |
| Advanced: Wi-Fi channel, hide Wi-Fi name | Radio options for the dongle's own network |
| Advanced: backlight level, invert colours, swap red/blue, screen offsets | Fixes for screens that look wrong |

The form is pre-filled with the project defaults. **The default Wi-Fi password is visible on the
public page and in `site/index.html` / `src/main.cpp`, so choose your own before installing.**
To change the defaults that the page shows, edit `DEFAULTS` near the bottom of `site/index.html`
and the matching values at the top of `src/main.cpp`.

### How it works
- `partitions.csv` adds a 4 KB data partition named `cfg` at flash offset `0xD000`.
- The installer downloads the published `firmware.bin`, writes your settings (plain `key=value`
  text starting with the line `WUSBCFG1`) into that blank area inside your browser, and flashes the
  result. Nothing is sent to any server.
- At boot the firmware reads the `cfg` partition. If it is blank or missing (for example when you
  flash from PlatformIO), the built-in defaults in `src/main.cpp` are used.
- The workflow checks that the `cfg` area is blank in every build, so a wrong partition layout is
  caught early.

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
