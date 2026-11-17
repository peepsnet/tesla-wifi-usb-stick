# Changelog

The firmware version is `FW_VERSION` near the top of `src/main.cpp`. It is shown on the stick's screen at
start-up, at the top of the stick's web page, and on the installer page.

## 0.13 (2026-11-17)
- **Network name:** the stick now calls itself `TeslaWiFiUSBStick-` plus the last 3 bytes of its MAC address
  (for example `TeslaWiFiUSBStick-D4F1A2`). That is the name your router lists, and
  `http://TeslaWiFiUSBStick-D4F1A2.local` opens the web page from your home network. The name is shown at the
  top of the web page. (The stick's own Wi-Fi network name is unchanged: choose it in the installer)
- **Firmware updates over Wi-Fi, keeping all your data.** New **Firmware update** panel on the web page. The new
  firmware is written to the second app slot and the stick restarts into it; settings, saved Wi-Fi networks and the
  SD card are never touched. Two ways in: the stick downloads it from GitHub itself (when it is on a home Wi-Fi with
  internet), or your phone/computer downloads it and uploads it to the stick, or you pick a downloaded
  `...-app.bin` file. The download is checked (size, MD5, ESP32-S3 app image) before the stick switches to it
- **Update-available icon:** a white arrow badge in the screen's title bar and a green arrow next to the title of the
  web page. The stick looks on GitHub 20 seconds after joining a home Wi-Fi and every 6 hours after that; the
  web page also looks using your own device's internet
- **Web installer: Update button** that flashes only the app (and resets the boot slot), so settings, saved
  networks and the card are kept. Use it once to get from 0.12 or older to 0.13; after that updates can
  happen over Wi-Fi
- Every build now also publishes `latest.json` and `firmware-app.bin` on the GitHub Pages site (which is where
  sticks look), and each release has a `...-app.bin` next to the `...-full.bin`

## 0.12 (2026-11-16)
- **The stick restarts after every new lock chime.** Testing in the car showed it only reads the new sound when
  the stick restarts; the 5 second eject from 0.11 was not enough and is gone
- **Button swapped:** a short press picks a random lock chime (and restarts); holding it for more than 5 seconds
  only restarts the stick. The screen says which will happen while the button is down
- The web page says the stick is restarting and comes back by itself

## 0.11 (2026-11-16)
- **New chime, longer eject:** after copying a lock chime the stick keeps the car's drive away for 5 seconds
  before giving it back, so the car treats it as a newly plugged-in drive and reads the new sound
- **The screen shows each step** of setting a chime: EJECT, COPY, the countdown, MOUNT, DONE (and PICK first for
  a random one)
- **Button:** a short press now restarts the stick (the car sees it unplugged and plugged in again); holding it
  for 2 seconds picks the random lock chime. The screen says which will happen while the button is down

## 0.10 (2026-11-16)
- **First Run on the screen** uses large type, and the countdown is 15 seconds
- **First Run without a screen** no longer starts by itself: hold the BOOT button for more than 5 seconds. The
  web page and README explain this, and to wait for the stick to appear as a USB drive before unplugging
- **Show password** tick box when joining a Wi-Fi network on the web page
- **Saved Wi-Fi networks are joined in every mode**, not only in edit mode
- **Edit mode shows the sound library to a computer** on the USB port as a drive. The web page can take it back
  (**Use the library here instead**); ejecting it on the computer does the same. Wi-Fi mode is now called edit mode
- A set-up card is recognised by its layout, not the `CHIME_STORE` name, so renaming the library on a computer is fine
- **The screen shows how full the sound library is** instead of the car's drive
- **Icon buttons** on the web page (upload, new folder, play, make lock chime, rename, delete) with a legend

## 0.9 (2026-11-16)
- **Fixed: First Run ran again after every restart, and made partitions of the wrong size.** The stick measured
  the first partition instead of the whole card, so it split that space only (half and half when it was 4 GB or
  less) and then failed to recognise its own layout. It now uses the real card size
- A card whose storage partition does not reach the end of the card (as left by 0.7 and 0.8) counts as not set
  up, so First Run formats it again, correctly. **Sounds stored on such a card are erased**
- **New screen layout:** coloured header bar (orange while the car has the drive, cyan in edit mode) with the
  version, a usage bar for the car's drive (green, orange from 80%, red from 95%), the mode, whether the address
  is on the stick's own Wi-Fi or your home Wi-Fi, and the address centred at the bottom

## 0.8 (2026-11-16)
- **First Run:** every card now uses the two-part layout. A card that doesn't have it (new, exFAT, used elsewhere)
  is set up automatically when the stick starts: a 4 GB `Boombox` drive for the car and the rest as the
  `CHIME_STORE` sound library. **This erases the card without asking**, after a 10 second countdown on the screen
  and the web page (unplug to cancel). The web page shows the progress
- Cards the stick could not mount before (unformatted, exFAT) are now set up too
- A card that can't be read is reported as a card error, never formatted
- **Self-repair:** if the car's drive can't be mounted at start-up, the stick re-creates just the `Boombox`
  partition and puts the last lock chime back. The sound library is not touched
- **Car drive tools** on the web page: a button that erases and re-creates the `Boombox` drive the same way
- The stick keeps a copy of the current lock chime in the library (hidden file `.LockChime.wav`) for this
- The Boombox drive is always 4 GB (half the card on small cards); the **SD card setup** section and its size
  choice are gone from the web page
- The stick always starts as a USB drive; the installer's **When the dongle is plugged in** setting is removed

## 0.7 (2026-11-16)
- **Sound library on a split card:** the `CHIME_STORE` partition is mounted by the stick, and the web page has
  **Sound library** and **Car drive** tabs. Upload, play, rename, delete and organise sounds in the library at any
  time, even while the car is using its drive (USB mode)
- **Make Lock Chime from the library** works in USB mode too: the stick ejects the car's drive for a moment, writes
  `LockChime.wav`, and gives the drive back
- **BOOT button** picks its random chime from the library on a split card (the car's drive is only touched for the
  copy)
- Cards that are not split work as before (one drive, edited in Wi-Fi mode)
- **New screen layout:** `Tesla WiFi USB Stick V0.7`, how full the car's drive is (`42% of 4GB Used`), and the IP
  address to open (your router's when joined, otherwise the stick's own). The title is orange while the car is
  using the drive and cyan in Wi-Fi mode
- **Installer page footnote** with the firmware version it installs, plus links to the source and releases

## 0.6 (2026-11-16)
- **GitHub releases:** every release has a ready-to-flash `tesla-wifi-usb-stick-vX.Y-full.bin` attached (one image,
  flashed at address 0x0). Made with Actions, Release, Run workflow; notes come from this file
- Shared build script (`tools/build-merged.sh`) used by the web installer and the releases
- No firmware changes apart from the version number

## 0.5 (2026-11-16)
First numbered version. Everything built so far:

- **USB drive + Wi-Fi web page** on the LilyGo T-Dongle S3: upload, download, rename, delete files and make
  folders. USB mode (car owns the card) and Wi-Fi mode (stick owns the card); ejecting the drive on the host
  switches to Wi-Fi mode
- **Tesla lock chime tools:** Play button on every `.wav`, Make Lock Chime (copies to `/LockChime.wav`, keeps the
  original, warns about non-PCM, non-16-bit, over 1 MB or very long files), Tesla guide on the web page and in
  the README
- **BOOT button** picks a random 16-bit PCM `.wav` under the size limit and makes it the lock chime
- **Wi-Fi client:** join your router (scan, or type a hidden network), up to 8 saved networks, delete them
  individually, auto-join the strongest one in range
- **Screen** (optional): mode, IP address, connected clients, free space; large IP in client mode
- **Drive name** "Boombox" (FAT volume label), changeable
- **Web installer** on GitHub Pages with settings written at flash time: Wi-Fi name/password, flip screen,
  drive name, start mode, chime size limit, screen tuning, dongle with or without a screen
- **Split card (experimental, step 1):** erase the card and split it into a `Boombox` partition (4 GB, 8 GB or
  half) that the car sees and a `CHIME_STORE` partition the car never sees; survives the car reformatting its
  drive
