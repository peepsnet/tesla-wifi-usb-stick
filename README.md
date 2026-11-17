# Wi-Fi USB Stick for Tesla Lock Chimes (LilyGo T-Dongle S3)

A **LilyGo T-Dongle S3** that acts as a USB drive in your Tesla, so you can **change the custom lock
chime from your phone** instead of pulling a USB drive out of the car and walking it to a computer.

Leave the stick in the car's USB port. Join its Wi-Fi, listen to your `.wav` files, tap
**Make Lock Chime**, and the file is copied to `LockChime.wav` ready for the car. (It also works as
a general-purpose Wi-Fi USB drive for other devices.)

**Current version: 0.15.** See [CHANGELOG.md](CHANGELOG.md) for what changed in each version.

## Features
- Looks like a normal USB drive (backed by the microSD card) to a car, PC, printer, etc.
- Web page over the dongle's own Wi-Fi: upload, download, rename, delete files, make folders
- **Play** button on every `.wav` so you can listen before choosing
- **Make Lock Chime** button on every `.wav`: copies it to `/LockChime.wav` (the original is kept) and
  warns you if the file doesn't look like something the car will play
- Button on the stick: a short press picks a random lock chime from the library (16-bit PCM, under the size limit)
  and restarts; holding it for more than 5 seconds only restarts the stick (the web page has a **Restart stick** button for the same)
- **Firmware updates over Wi-Fi from the web page** that keep your settings, saved networks and files, with an
  **update-available icon** on the screen and the page (see [Updating the firmware](#updating-the-firmware))
- Calls itself `TeslaWiFiUSBStick-<last 3 bytes of the MAC>` (for example `TeslaWiFiUSBStick-D4F1A2`) on your
  router and at `http://TeslaWiFiUSBStick-D4F1A2.local`
- Joins your home Wi-Fi too, in every mode: scan, enter a password, or type a hidden network name.
  Up to 8 networks are remembered and can be deleted individually from the web page
- Screen (if fitted) shows the mode, how full the sound library is (with a bar), which Wi-Fi the address is on, and
  the address to open ([mock-up](docs/screen-mockup.png))
- Drive is named "Boombox" (changeable)
- Two-part card: a 4 GB Boombox drive for the car and a hidden sound library for the stick. Any card is set up
  automatically the first time it is used (**this erases it**, see First Run below)

## Install (no tools needed)
Open the installer page (GitHub Pages for this repository) in **Chrome or Edge on a desktop**
and follow the steps. You must hold the dongle's button while plugging it in so it enters
flashing mode.

## Tesla lock chime guide

### What the car needs
Tesla doesn't publish every detail, and owner guides don't always agree, so treat your owner's manual
as the final word. These are the requirements that most guides agree on:

| Item | Requirement |
|---|---|
| File name | Exactly `LockChime.wav` (capital L and C). Beware `LockChime.wav.wav` from hidden extensions |
| Format | A real **WAV** file (PCM). An MP3 renamed to `.wav` does not work. The safe choice is **16-bit PCM, 44.1 kHz**, mono or stereo |
| Size | **Under 1 MB**. That is roughly 5 seconds of 16-bit 44.1 kHz stereo. 1-3 seconds is plenty |
| Location | The **root** of the drive, not inside a folder. (A few sites say `Boombox/LockChime.wav`; if the root doesn't work for you, try that) |
| How many | Only **one** custom sound is used at a time |
| Drive format | **FAT32** or **exFAT**, not NTFS. FAT32 is best with this stick |
| Car | The Lock Sound feature arrived in software **2023.44** and needs the external pedestrian-warning speaker. Availability varies by model, market and software |
| USB port | One that carries **data** (the glovebox port on newer cars). Some console ports are charge-only |
| In the car | Open the **Lock Sound** setting and choose the USB/custom sound. On many versions that is Toybox, Boombox, Lock Sound, USB |

### Changing the chime with this stick
1. Join the stick's Wi-Fi and open `http://192.168.4.1`.
2. Upload `.wav` files to the **Sound library**, press **Play** to listen, then **Make Lock Chime** on the one
   you want. The page shows its format and warns about anything unusual (not PCM, not 16-bit, too big, very long).
3. Lock the car and listen. Or **press the stick's button** for a random one from the library.

To set the chime, the stick takes the drive from the car, writes the file and then **restarts**. The car only
reads the new sound when the stick looks unplugged and plugged in again; taking the drive away and giving it back
is not enough. The screen shows each step: EJECT, COPY, DONE, then the restart. Your phone may drop off the
stick's Wi-Fi for a few seconds while it restarts.

**Holding the button for more than 5 seconds** restarts the stick without changing the chime.

### If the car still plays the old sound
- Re-open the Lock Sound setting and select USB again. Some owners report the car keeps the old file until they
  pick another sound and then USB again. Reports differ on whether the car re-reads the file every time or
  keeps a copy, so test this on your own car.
- Check the exact name, the size, that it is a real PCM WAV, and that it sits at the root of the drive.
- Try the other USB port and make sure Lock Sound is switched on.

### Limits and cautions
- **Don't use the stick as your dashcam or Sentry drive.** The ESP32-S3's USB is full speed only, which is too
  slow for recording. If the car offers to **format** the drive, decline: that would erase the car's drive
  (the sound library would survive).
- The drive disappears from the car for a moment whenever you switch between Car Mode and PC Mode.
- The web page has no login. Anyone who can join the stick's Wi-Fi can change the files.

## First Run and the card layout
The stick always uses the card in two parts:

| Partition | Size | Who uses it |
|---|---|---|
| `Boombox` (FAT32) | 4 GB (half the card if it is too small for that) | The car. This is the only part it can see |
| `CHIME_STORE` (FAT32) | Everything else | The stick only. The car never sees it |

**First Run:** when the stick starts with a card that doesn't have this layout (brand new, exFAT, used in a camera,
anything), the card is erased and set up. **This erases everything on the card.**

- **Stick with a screen:** the screen and the web page count down 15 seconds, then it starts by itself. Unplug the
  stick during the countdown to keep the card as it is.
- **Stick without a screen:** nothing happens until you **hold the button on the stick down for more than
  5 seconds**. The web page (`http://192.168.4.1` on the stick's Wi-Fi) says the same and shows the progress.

The format takes a few minutes and the stick restarts when done. The car or computer is not given a drive until
the card is set up, so **without a screen, plug the stick into a computer and wait until it shows up as a USB
drive before unplugging it**: that is the sign that setup has finished.

A card that is already set up is never fully erased again. If the stick can't read the card at all, it reports a
card error instead of formatting it.

**Repairing the car's drive:** every time a lock chime is set, the stick keeps a copy of it in the library (a
hidden file, `.LockChime.wav`). If the car's drive can't be mounted when the stick starts, the stick erases and
re-creates only the `Boombox` partition and puts that chime back. You can do the same by hand with **Car drive
tools**, **Erase and re-create the Boombox drive** on the web page. The sound library is never touched by either.

The car is shown a smaller drive that ends where `CHIME_STORE` begins, with a partition table that lists only
`Boombox`. If the car (or a PC) reformats or repartitions its drive, the stick keeps the storage partition's entry,
so `CHIME_STORE` and its files survive. Put the card in a PC card reader and you will see both partitions.

### Modes
| | Car (USB port) sees | Web page can edit |
|---|---|---|
| **Car Mode** (normal, and always how the stick starts) | the `Boombox` drive. `CHIME_STORE` is hidden | the sound library |
| **PC Mode** | the sound library (`CHIME_STORE`), as a drive for a computer. `Boombox` is hidden | the `Boombox` drive |

Each part of the card is only ever used by one side at a time; that is what keeps the files safe. In PC Mode,
plug the stick into a computer to copy sounds straight onto the library. To edit the library from the web page
while in PC Mode, eject the drive on the computer and press **Use the library here instead**. Ejecting a drive
on the computer always makes the stick take that drive back.

The stick's web page has two tabs:
- **Sound library** (`CHIME_STORE`): your sounds. Upload, play, rename, delete and organise them at any time, even
  while the car is using its drive.
- **Car drive** (`Boombox`): what the car sees. Only editable in PC Mode, when the stick (not the car) owns it.

**Make Lock Chime** on a library sound copies it to `LockChime.wav` on the car drive. If the car is using the drive,
the stick takes it away, copies the file and restarts.

## Updating the firmware
Updates never touch your settings (Wi-Fi name/password, drive name, ...), your saved Wi-Fi networks or the SD card
(sounds included). That works because the flash has **two app slots** (`partitions.csv`): the update is written to
the slot that is not running, checked, and only then does the stick switch to it. Settings live in their own
areas (`cfg`, `nvs`) that an update never writes.

**The update-available icon.** When a newer version than the one on the stick has been published, a white arrow
badge appears in the screen's title bar and a green arrow next to the title of the web page. The stick looks
20 seconds after it joins a home Wi-Fi that has internet, then every 6 hours. (The web page also looks, using
the internet of the phone or computer you opened it on, so the arrow can appear there even if the stick has no
internet itself.) The stick never installs anything on its own: you press **Install**.

**Over Wi-Fi (firmware 0.13 or newer):** open the web page, **Firmware update**, **Check for updates**, **Install**.
- *The stick has internet* (it joined your home Wi-Fi): the stick downloads the firmware from GitHub itself.
  Your phone only watches the progress.
- *Only the phone/computer has internet:* the page downloads the firmware from GitHub and hands it to the stick.
- *Nobody has internet at the car:* download the `...-app.bin` from the
  [releases](https://github.com/peepsnet/tesla-wifi-usb-stick/releases) beforehand and choose it under
  **Install this file**.

The stick then shows the progress on its screen, restarts into the new firmware (so the car sees it unplugged
and plugged in again) and the page reloads by itself. If the download or the check fails, nothing is changed.

**Over USB with the web installer:** the installer page has **Update firmware (keeps my data)**. It flashes only
the app (and resets the boot slot), unlike **Install firmware**, which writes the whole flash including your
settings. **Sticks that run 0.12 or older have no Wi-Fi update yet, so use this once to get to 0.13.** It is also the
way back if a firmware ever fails to start (hold the button while plugging in, then update).

**Where the update is hosted:** on this repository's GitHub. Every push to `main` makes the workflow publish, on
the GitHub Pages site, `latest.json` (version, size, MD5) and `firmware-app.bin`, next to the web installer; each
release also carries `...-app.bin`. The stick reads `UPDATE_BASE_URL` in `src/main.cpp` (already set to this
repository's Pages site; change it if you fork) over HTTPS and checks GitHub's certificate. GitHub Pages can take
a few minutes to show a new file.

Limits: a release that changes `partitions.csv` cannot be installed this way (the release notes would say so and
you would use **Install firmware**). The Wi-Fi update needs a working Wi-Fi link: if the stick loses power halfway
the old firmware keeps running, because the new one is only switched to at the very end.

## Settings you can choose before flashing
The installer page has a form. Whatever you pick is written into a small "cfg" area of the
dongle's flash together with the firmware:

| Setting | What it does |
|---|---|
| My dongle has a screen | Untick for the version without the LCD; the firmware then skips the screen |
| Wi-Fi name / password | The dongle's own Wi-Fi network (password 8-63 characters) |
| Flip screen? | No = normal, Yes = rotate the screen 180 degrees |
| Drive name | The name shown for the drive on your computer (max 11 characters) |
| Largest random lock chime (KB) | The button only picks `.wav` files smaller than this |
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
- Only one side can use each part of the card at a time (see Modes above).
- The web page has no login. Anyone who can join the dongle's Wi-Fi (or your home Wi-Fi when the
  dongle is connected to it) can change the files.
- Any microSD card works; First Run formats it (see above). Cards of 4 GB or less are split in half.

## Building it yourself
1. Install VS Code + the PlatformIO extension.
2. Open this folder and run **PlatformIO: Build** (or `pio run`).
3. Hold the dongle's button while plugging it in, then **PlatformIO: Upload**.

## Releasing a new version
1. Change `#define FW_VERSION "0.5"` near the top of `src/main.cpp` (for example to `"0.6"`). Sticks compare this
   number with the one in `latest.json` to decide whether to show the update icon, so it must go **up** (`0.9` is
   older than `0.12`).
2. Add a `## 0.6 (date)` section at the top of `CHANGELOG.md`, and update "Current version" at the top of
   this README.
3. Commit and push to `main`. That is all: one workflow builds the firmware once, rebuilds the web installer
   (it shows `0.6 (build abc1234)`) and, because there is no tag `v0.6` yet, also creates that tag and publishes a
   release with the `CHANGELOG.md` notes and two files attached: `tesla-wifi-usb-stick-v0.6-full.bin` (first install)
   and `tesla-wifi-usb-stick-v0.6-app.bin` (update that keeps the data). Installed sticks also see the new version
   through `latest.json` on the Pages site and show the update icon.

Pushes that don't change `FW_VERSION` only update the web installer; a version is never released twice. If the
release step fails (for example `CHANGELOG.md` has no section for the version), the installer is still updated:
fix the problem and push again, or use **Actions**, **Run workflow**.

## Flashing a release file
Each [release](https://github.com/peepsnet/tesla-wifi-usb-stick/releases) has a `...-full.bin` attached. It holds
everything (bootloader, partition table, app) and goes at address **0x0**. It uses the default settings; use the
web installer instead if you want your own Wi-Fi name and password.

1. Hold the button on the dongle while plugging it in.
2. Open [esptool-js](https://espressif.github.io/esptool-js/) in Chrome or Edge, click **Connect**, pick the dongle.
3. Flash address `0x0`, choose the `.bin`, click **Program**. Then unplug and re-plug without holding the button.

Command line: `esptool.py --chip esp32s3 write_flash 0x0 tesla-wifi-usb-stick-v0.6-full.bin`

## How the web installer and releases are built
`.github/workflows/build-and-deploy.yml` does both. It calls `tools/build-merged.sh`, which builds with PlatformIO
and merges everything into one image, then publishes that image with the web installer to GitHub Pages and, for a
new version, attaches the same file to a GitHub release.

The installer page uses [ESP Web Tools](https://esphome.github.io/esp-web-tools/).

Besides the installer and `firmware.bin`, the same build publishes `firmware-app.bin`, `otadata.bin`, `latest.json`
and `manifest-update.json` for the update paths above (see the top of `tools/build-merged.sh`).

If a flashed dongle doesn't boot, try changing `--flash_mode dio` to `--flash_mode qio` in
`tools/build-merged.sh`.
