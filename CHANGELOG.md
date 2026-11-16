# Changelog

The firmware version is `FW_VERSION` near the top of `src/main.cpp`. It is shown on the stick's screen at
start-up, at the top of the stick's web page, and on the installer page.

## 0.7 (2026-11-16)
- **Sound library on a split card:** the `CHIME_STORE` partition is mounted by the stick, and the web page has
  **Sound library** and **Car drive** tabs. Upload, play, rename, delete and organise sounds in the library at any
  time, even while the car is using its drive (USB mode)
- **Make Lock Chime from the library** works in USB mode too: the stick ejects the car's drive for a moment, writes
  `LockChime.wav`, and gives the drive back
- **BOOT button** picks its random chime from the library on a split card (the car's drive is only touched for the
  copy)
- Cards that are not split work as before (one drive, edited in Wi-Fi mode)

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
