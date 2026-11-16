/*
 * Wi-Fi USB Stick  (ESP32-S3, Arduino-ESP32 core 3.x)
 * Version: see FW_VERSION below, and CHANGELOG.md
 *
 * Plugs into a USB host and looks like a normal flash drive (backed by a
 * microSD card). Join the stick's Wi-Fi network, open http://192.168.4.1
 * and upload / rename / delete / download files.
 *
 * IMPORTANT: only ONE side may own the card at a time, otherwise the FAT
 * filesystem gets corrupted. So the stick has two modes:
 *
 *   USB mode  - the host owns the card. Web page is read-only (disabled).
 *   Wi-Fi mode - the ESP32 owns the card. The host sees the drive as "ejected".
 *
 * Switch modes with the web page button, or by ejecting the drive in the
 * host OS (-> switches to Wi-Fi mode).
 *
 * Settings: the values in the config block below are only DEFAULTS. The web
 * installer can write a small "cfg" block into flash (Wi-Fi name/password,
 * screen flip, drive name, ...) which overrides them at boot (see loadConfig).
 *
 * BOOT button: picks a random .wav (smaller than the size limit) from
 * anywhere on the card and copies it to /LockChime.wav. The original file is
 * left in place. If the drive is in USB mode, the stick briefly takes the card
 * back from the host (the host sees an eject/reinsert) to do the copy.
 *
 * Split card (optional, set up from the web page): the SD card is divided into a
 * small "Boombox" partition that the car sees and a "CHIME_STORE" partition the
 * car never sees. See the CARD LAYOUT section.
 *
 * Wi-Fi: the stick always runs its own access point. In Wi-Fi mode it can
 * also join your router: use the "Wi-Fi network" panel on the web page to
 * scan, enter a password and connect. Credentials are saved in flash and
 * reused every time Wi-Fi mode is selected.
 *
 * Display (T-Dongle S3 with LCD):
 *   Wi-Fi AP mode     -> mode, stick IP, number of connected clients
 *   Wi-Fi client mode -> mode, IP address given by your router
 *   USB mode          -> mode, free space left on the card
 * Needs the "LovyanGFX" library by lovyan03 (declared in platformio.ini).
 * Free space in USB mode is counted by reading the card's FAT table
 * directly (FAT16/FAT32 only; exFAT shows "n/a").
 *
 * PlatformIO: see platformio.ini (uses the pioarduino platform for
 * Arduino-ESP32 core 3.x, USB-OTG/TinyUSB mode, CDC on boot disabled).
 * Serial then goes to the UART pins, not the USB port.
 *
 * Pins below are for the LilyGo T-Dongle S3. Change them for other boards.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <FS.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include "esp_partition.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "USB.h"
#include "USBMSC.h"

// ---------------- Config ----------------
const char* AP_SSID = "Tesla-USB-Stick";
const char* AP_PASS = "s3xyandiknowit";   // at least 8 characters
#define FW_VERSION "0.6"               // bump for every release (also listed in CHANGELOG.md)
#define START_IN_USB_MODE true
#define VOLUME_LABEL "Boombox"      // name shown for the drive (max 11 characters)
#define BUTTON_PIN 0                   // BOOT button on most S3 boards
#define MAX_CHIME_BYTES 1000000        // random lock chime only uses .wav files smaller than this

// SD card pins (1-bit SD_MMC). LilyGo T-Dongle S3 values:
#define SD_CLK 12
#define SD_CMD 16
#define SD_D0  14

// Display: ST7735 80x160 on the LilyGo T-Dongle S3 (with LCD).
// Set USE_DISPLAY to false for a board without a screen.
#define USE_DISPLAY  true
#define LCD_SCLK     5
#define LCD_MOSI     3
#define LCD_DC       2
#define LCD_CS       4
#define LCD_RST      1
#define LCD_BL       38
#define LCD_BL_ON    LOW      // backlight is active-low on this board (use HIGH if the screen stays dark)
#define LCD_ROTATION 3        // 1 or 3 = landscape (1 flips it 180 degrees)
// Panel tuning - only change these if the picture looks wrong:
#define LCD_OFFSET_X 26       // picture shifted/cut off at an edge? try 24..28
#define LCD_OFFSET_Y 1        // try 0..2
#define LCD_INVERT   true     // black and white swapped? set false
#define LCD_RGB_ORDER false   // red and blue swapped? set true
// ----------------------------------------

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ST7735S _panel;
  lgfx::Bus_SPI _bus;
public:
  LGFX(int offsetX, int offsetY, bool invert, bool rgbOrder) {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 27000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = LCD_SCLK;
      cfg.pin_mosi = LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = LCD_CS;
      cfg.pin_rst = LCD_RST;
      cfg.pin_busy = -1;
      cfg.panel_width = 80;
      cfg.panel_height = 160;
      cfg.offset_x = offsetX;
      cfg.offset_y = offsetY;
      cfg.offset_rotation = 0;
      cfg.readable = false;
      cfg.invert = invert;
      cfg.rgb_order = rgbOrder;
      cfg.dlen_16bit = false;
      cfg.bus_shared = false;
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};
LGFX* lcdPtr = nullptr;     // created in displayInit(), after the settings are loaded

USBMSC msc;
WebServer server(80);
SemaphoreHandle_t sdLock;
volatile bool usbMode = false;
volatile bool ejectRequested = false;
uint32_t sectorCount = 0;
uint64_t cardTotal = 0;
File uploadFile;

// Runtime settings. They start at the defaults from the config block at the top
// of this file and are overridden by the "cfg" flash block the web installer can
// write (see loadConfig() near the end of this file).
String   cfgSsid = AP_SSID;
String   cfgPass = AP_PASS;
String   cfgLabel = VOLUME_LABEL;
bool     cfgStartUsb = START_IN_USB_MODE;
uint32_t cfgMaxChime = MAX_CHIME_BYTES;
int      cfgRotation = LCD_ROTATION;
int      cfgOffX = LCD_OFFSET_X, cfgOffY = LCD_OFFSET_Y;
bool     cfgInvert = LCD_INVERT, cfgRgb = LCD_RGB_ORDER;
uint8_t  cfgBlOn = LCD_BL_ON;
bool     cfgHidden = false;     // hide the stick's own Wi-Fi network name
int      cfgChannel = 1;        // channel of the stick's own Wi-Fi network (1-13)
bool     cfgDisplay = USE_DISPLAY;   // false for a dongle without a screen

// Wi-Fi client (station) state
#define STA_TIMEOUT_MS 20000
Preferences prefs;
#define MAX_NETS 8
struct SavedNet { String ssid; String pass; bool hidden = false; };
SavedNet nets[MAX_NETS];             // remembered networks (kept in flash)
int netCount = 0;
String pendingSsid, pendingPass;     // being tried right now (started from the web page)
bool pendingHidden = false;
bool staPending = false;
uint32_t staStart = 0;
bool staScanActive = false;          // auto-join: a scan is running
bool staJoining = false;             // auto-join: connecting to a remembered network
uint32_t staNextScan = 0, staJoinStart = 0;

// USB-mode free-space counter (reads the FAT table directly)
volatile uint32_t usbWriteCount = 0;   // bumped on every host write
uint8_t  freeState = 0;                // 0 = unknown/scanning, 1 = valid, 2 = unsupported
uint64_t freeBytes = 0, fsTotalBytes = 0;
bool     scanActive = false, scanFat32 = true;
uint32_t scanFatStart = 0, scanSectors = 0, scanNext = 0, scanFree = 0;
uint32_t scanClusters = 0, scanSpc = 0;
uint32_t scanWriteMark = 0, lastCountSeen = 0, lastWriteMs = 0;
String staResult = "";           // "", "failed"

// ---------------- SD helpers ----------------
bool mountSD() {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  return SD_MMC.begin("/sdcard", true);   // true = 1-bit mode
}

// Raw one-sector access used by the card layout code below.
bool formatting = false;                  // true while the card is being split
static uint32_t fmtWrites = 0;
bool cardRead(uint32_t lba, uint8_t* buf) { return SD_MMC.readRAW(buf, lba); }
bool cardWrite(uint32_t lba, const uint8_t* buf) {
  if (formatting && (++fmtWrites & 127) == 0) delay(1);   // let other tasks run during a long format
  return SD_MMC.writeRAW((uint8_t*)buf, lba);
}
void formatProgress(int percent);         // shows progress on the screen (Display section)

// ==== CARD LAYOUT BEGIN ====
// Two-partition card layout ("split card"):
//
//   sector 0         MBR (partition table)
//   2048 ...         partition 1: "Boombox"     FAT32, the part the car sees
//   aligned ... end  partition 2: "CHIME_STORE" FAT32, the stick's own storage
//
// The car (USB host) is shown a smaller disk that ends where CHIME_STORE starts,
// and a copy of the MBR with only partition 1 in it, so it can't see or touch
// the storage partition. A card that was never split works exactly as before.
//
// This section only uses three hooks, defined before it:
//   bool cardRead(uint32_t lba, uint8_t* buf)          read one 512-byte sector
//   bool cardWrite(uint32_t lba, const uint8_t* buf)   write one 512-byte sector
//   void formatProgress(int percent)                   progress while formatting

#define BOOMBOX_START  2048UL          // 1 MB, the usual first-partition offset
#define PART_ALIGN     8192UL          // 4 MB alignment (good for SD card speed)
#define STORE_LABEL    "CHIME_STORE"   // 11 characters, marks a split card
#define MIN_STORE_SECT 2097152UL       // keep at least 1 GB for storage

struct CardLayout {
  bool     split = false;
  uint32_t total = 0;                  // sectors on the card
  uint32_t hostSectors = 0;            // sectors the car is shown
  uint32_t bbStart = 0, bbSize = 0;    // Boombox partition
  uint32_t stStart = 0, stSize = 0;    // storage partition
  uint8_t  storeEntry[16];             // MBR entry for the storage partition
};
CardLayout layout;

static uint32_t clRd32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void clWr16(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; }
static void clWr32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static bool clIsFat(uint8_t t) { return t == 0x0B || t == 0x0C; }

// Work out whether the card is split, and what the car should be shown.
void readLayout(uint32_t totalSectors) {
  layout = CardLayout();
  layout.total = totalSectors;
  layout.hostSectors = totalSectors;
  uint8_t b[512];
  if (totalSectors == 0 || !cardRead(0, b) || b[510] != 0x55 || b[511] != 0xAA) return;

  const uint8_t* e2 = b + 462;                       // second partition entry
  uint32_t s = clRd32(e2 + 8), n = clRd32(e2 + 12);
  if (!clIsFat(e2[4]) || s < BOOMBOX_START || n == 0 || s + n > totalSectors || s + n < s) return;

  uint8_t v[512];                                    // is it really our storage partition?
  if (!cardRead(s, v) || v[510] != 0x55 || v[511] != 0xAA) return;
  if (memcmp(v + 82, "FAT32   ", 8) != 0 || memcmp(v + 71, STORE_LABEL, 11) != 0) return;

  layout.split = true;
  layout.stStart = s;
  layout.stSize = n;
  layout.hostSectors = s;                            // the car's disk ends here
  memcpy(layout.storeEntry, e2, 16);
  const uint8_t* e1 = b + 446;
  uint32_t s1 = clRd32(e1 + 8), n1 = clRd32(e1 + 12);
  if (clIsFat(e1[4]) && s1 > 0 && n1 > 0 && s1 + n1 <= s) {
    layout.bbStart = s1;
    layout.bbSize = n1;
  } else {                                           // the car reformatted it its own way
    layout.bbStart = 0;
    layout.bbSize = s;
  }
}

// ---- What the car sees ----
// Sector 0 is shown with partition entries 2-4 blanked, and the disk ends at
// the storage partition. If the car writes sector 0 (for example when it
// formats the drive), entries 2-4 are put back so the storage partition survives.
bool hostRead(uint32_t lba, uint8_t* buf) {
  if (lba >= layout.hostSectors) return false;
  if (!cardRead(lba, buf)) return false;
  if (layout.split && lba == 0) memset(buf + 462, 0, 48);
  return true;
}

bool hostWrite(uint32_t lba, const uint8_t* buf) {
  if (lba >= layout.hostSectors) return false;
  if (!(layout.split && lba == 0)) return cardWrite(lba, buf);
  uint8_t m[512];
  memcpy(m, buf, 512);
  memcpy(m + 462, layout.storeEntry, 16);
  memset(m + 478, 0, 32);
  m[510] = 0x55;
  m[511] = 0xAA;
  return cardWrite(0, m);
}

// ---- Formatting ----
static uint8_t clZero[512];

static bool clZeroRange(uint32_t lba, uint32_t count, uint64_t& done, uint64_t all) {
  for (uint32_t i = 0; i < count; i++) {
    if (!cardWrite(lba + i, clZero)) return false;
    done++;
    if ((done & 1023) == 0) formatProgress((int)(done * 100 / all));
  }
  return true;
}

struct Fat32Plan {
  uint32_t start, size, rsvd, fatSz, spc, clusters;
};

// Lay out a FAT32 volume of `size` sectors starting at `start`. Returns false
// if the partition is too small for FAT32.
static bool planFat32(uint32_t start, uint32_t size, Fat32Plan& p) {
  p.start = start;
  p.size = size;
  // Microsoft's usual cluster sizes: up to 8 GB 4 KB, 16 GB 8 KB, 32 GB 16 KB, larger 32 KB
  p.spc = size <= 16777216UL ? 8 : size <= 33554432UL ? 16 : size <= 67108864UL ? 32 : 64;
  for (;;) {
    p.rsvd = 32;
    uint32_t tmp2 = 128 * p.spc + 1;                 // (256 * spc + 2 FATs) / 2
    p.fatSz = (size - p.rsvd + tmp2 - 1) / tmp2;
    uint32_t dataStart = start + p.rsvd + 2 * p.fatSz;
    p.rsvd += (PART_ALIGN - dataStart % PART_ALIGN) % PART_ALIGN;   // align the data area
    if (size <= p.rsvd + 2 * p.fatSz) return false;
    p.clusters = (size - p.rsvd - 2 * p.fatSz) / p.spc;
    if (p.clusters >= 65525 || p.spc == 1) break;
    p.spc /= 2;                                      // small partition: smaller clusters
  }
  return p.clusters >= 65525 && p.clusters < 0x0FFFFFF5UL && (uint64_t)p.fatSz * 128 >= p.clusters + 2;
}

static uint64_t planWrites(const Fat32Plan& p) { return (uint64_t)p.rsvd + 2ULL * p.fatSz + p.spc; }

// Write an empty FAT32 file system. The boot sector is written last, so a
// half-finished format never looks like a valid file system.
static bool writeFat32(const Fat32Plan& p, const char* label, uint32_t volId, uint64_t& done, uint64_t all) {
  uint8_t s[512];
  char name[11];
  memset(name, ' ', 11);
  for (int i = 0; i < 11 && label[i]; i++) name[i] = label[i];

  if (!clZeroRange(p.start, p.rsvd, done, all)) return false;          // reserved area
  uint32_t fat1 = p.start + p.rsvd;
  for (int f = 0; f < 2; f++) {                                         // both FATs
    uint32_t fs = fat1 + f * p.fatSz;
    memset(s, 0, 512);
    clWr32(s + 0, 0x0FFFFFF8);                                          // media byte entry
    clWr32(s + 4, 0x0FFFFFFF);
    clWr32(s + 8, 0x0FFFFFFF);                                          // root directory: one cluster
    if (!cardWrite(fs, s)) return false;
    done++;
    if (!clZeroRange(fs + 1, p.fatSz - 1, done, all)) return false;
  }
  uint32_t root = fat1 + 2 * p.fatSz;                                  // cluster 2 = root directory
  memset(s, 0, 512);
  memcpy(s, name, 11);
  s[11] = 0x08;                                                         // volume label entry
  if (!cardWrite(root, s)) return false;
  done++;
  if (!clZeroRange(root + 1, p.spc - 1, done, all)) return false;

  memset(s, 0, 512);                                                    // FSInfo (+ backup)
  clWr32(s + 0, 0x41615252);
  clWr32(s + 484, 0x61417272);
  clWr32(s + 488, p.clusters - 1);                                      // free clusters
  clWr32(s + 492, 3);                                                   // next free cluster
  s[510] = 0x55; s[511] = 0xAA;
  if (!cardWrite(p.start + 1, s) || !cardWrite(p.start + 7, s)) return false;
  memset(s, 0, 512);
  s[510] = 0x55; s[511] = 0xAA;
  if (!cardWrite(p.start + 2, s) || !cardWrite(p.start + 8, s)) return false;

  memset(s, 0, 512);                                                    // boot sector
  s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
  memcpy(s + 3, "MSWIN4.1", 8);
  clWr16(s + 11, 512);
  s[13] = (uint8_t)p.spc;
  clWr16(s + 14, p.rsvd);
  s[16] = 2;                                                            // number of FATs
  s[21] = 0xF8;                                                         // fixed disk
  clWr16(s + 24, 63);
  clWr16(s + 26, 255);
  clWr32(s + 28, p.start);                                              // hidden sectors
  clWr32(s + 32, p.size);
  clWr32(s + 36, p.fatSz);
  clWr32(s + 44, 2);                                                    // root cluster
  clWr16(s + 48, 1);                                                    // FSInfo sector
  clWr16(s + 50, 6);                                                    // backup boot sector
  s[64] = 0x80;
  s[66] = 0x29;
  clWr32(s + 67, volId);
  memcpy(s + 71, name, 11);
  memcpy(s + 82, "FAT32   ", 8);
  s[510] = 0x55; s[511] = 0xAA;
  if (!cardWrite(p.start + 6, s)) return false;                         // backup copy first
  return cardWrite(p.start, s);
}

static void clEntry(uint8_t* e, uint32_t start, uint32_t size) {
  memset(e, 0, 16);
  e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;                                // CHS unused (LBA only)
  e[4] = 0x0C;                                                          // FAT32 (LBA)
  e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
  clWr32(e + 8, start);
  clWr32(e + 12, size);
}

// Size choice: 0 = 4 GB Boombox, 1 = 8 GB, 2 = half the card.
uint32_t boomboxSectors(uint32_t total, int choice) {
  uint32_t usable = total > BOOMBOX_START ? total - BOOMBOX_START : 0;
  uint32_t want = choice == 0 ? 8388608UL : choice == 1 ? 16777216UL : usable / 2;
  if (want + MIN_STORE_SECT + PART_ALIGN > usable) want = usable / 2;   // small card: split it in half
  return want - want % PART_ALIGN;
}

// Erase the whole card and split it. Returns "" on success, or what went wrong.
// volIdSeed should be something random (for example esp_random()).
const char* formatSplitCard(uint32_t total, int choice, uint32_t volIdSeed) {
  uint32_t bb = boomboxSectors(total, choice);
  uint32_t st = BOOMBOX_START + bb;
  st += (PART_ALIGN - st % PART_ALIGN) % PART_ALIGN;
  if (st >= total) return "Card too small";
  uint32_t stSize = total - st;

  Fat32Plan pb, ps;
  if (!planFat32(BOOMBOX_START, bb, pb)) return "Card too small for the Boombox partition";
  if (!planFat32(st, stSize, ps)) return "Card too small for the storage partition";

  uint64_t all = BOOMBOX_START + planWrites(pb) + planWrites(ps) + 64, done = 0;
  formatProgress(0);

  // 1. Remove the old partition table and any GPT (start and end of the card),
  //    so nothing half-formatted is ever recognised.
  if (!clZeroRange(0, BOOMBOX_START, done, all)) return "Write error";
  if (!clZeroRange(total - 34, 34, done, all)) return "Write error";

  // 2. The two empty FAT32 file systems.
  if (!writeFat32(pb, "Boombox", volIdSeed, done, all)) return "Write error (Boombox)";
  if (!writeFat32(ps, STORE_LABEL, volIdSeed ^ 0x5A5A1234UL, done, all)) return "Write error (storage)";

  // 3. The partition table, last.
  uint8_t m[512];
  memset(m, 0, 512);
  clWr32(m + 440, volIdSeed ^ 0xA5A5A5A5UL);                             // disk signature
  clEntry(m + 446, BOOMBOX_START, bb);
  clEntry(m + 462, st, stSize);
  m[510] = 0x55; m[511] = 0xAA;
  if (!cardWrite(0, m)) return "Write error (partition table)";
  formatProgress(100);
  return "";
}
// ==== CARD LAYOUT END ====

// ---------------- Wi-Fi client helpers ----------------
// Up to MAX_NETS networks are remembered in flash. In Wi-Fi mode the stick scans
// now and then and joins the strongest remembered network that is in range.
static int findNet(const String& ssid) {
  for (int i = 0; i < netCount; i++) {
    if (nets[i].ssid == ssid) return i;
  }
  return -1;
}

void saveWifiPrefs() {
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.putInt("n", netCount);
  for (int i = 0; i < netCount; i++) {
    String idx = String(i);
    prefs.putString(("s" + idx).c_str(), nets[i].ssid);
    prefs.putString(("p" + idx).c_str(), nets[i].pass);
    prefs.putBool(("h" + idx).c_str(), nets[i].hidden);
  }
  prefs.end();
}

void loadWifiPrefs() {
  netCount = 0;
  prefs.begin("wifi", false);
  int n = prefs.getInt("n", -1);
  if (n < 0) {                                   // nothing saved, or the old single-network format
    String old = prefs.getString("ssid", "");
    if (old.length() > 0) {
      nets[0].ssid = old;
      nets[0].pass = prefs.getString("pass", "");
      nets[0].hidden = false;
      netCount = 1;
    }
    prefs.end();
    if (netCount > 0) saveWifiPrefs();           // convert to the new format
    return;
  }
  if (n > MAX_NETS) n = MAX_NETS;
  int j = 0;
  for (int i = 0; i < n; i++) {
    String idx = String(i);
    String ssid = prefs.getString(("s" + idx).c_str(), "");
    if (ssid.length() == 0) continue;
    nets[j].ssid = ssid;
    nets[j].pass = prefs.getString(("p" + idx).c_str(), "");
    nets[j].hidden = prefs.getBool(("h" + idx).c_str(), false);
    j++;
  }
  netCount = j;
  prefs.end();
}

// Remember a network (or update its password). Returns false if the list is full.
bool addOrUpdateNet(const String& ssid, const String& pass, bool hidden) {
  int k = findNet(ssid);
  if (k < 0) {
    if (netCount >= MAX_NETS) return false;
    k = netCount++;
    nets[k].ssid = ssid;
  }
  nets[k].pass = pass;
  nets[k].hidden = hidden;
  saveWifiPrefs();
  return true;
}

bool deleteNet(const String& ssid) {
  int k = findNet(ssid);
  if (k < 0) return false;
  for (int i = k; i < netCount - 1; i++) nets[i] = nets[i + 1];
  netCount--;
  nets[netCount].ssid = "";
  nets[netCount].pass = "";
  nets[netCount].hidden = false;
  saveWifiPrefs();
  return true;
}

// Called when Wi-Fi mode is entered: look for a remembered network soon.
void staConnectSaved() {
  staScanActive = false;
  staJoining = false;
  staNextScan = 0;
}

void staDisconnect() {
  staPending = false;
  staScanActive = false;
  staJoining = false;
  WiFi.disconnect(false, false);   // keep the access point running
}

// Runs from loop(): in Wi-Fi mode, when not connected to a router, scan every
// so often and join the strongest remembered network that is visible.
void staAutoJoinTick() {
  if (usbMode || staPending || netCount == 0) return;
  if (WiFi.status() == WL_CONNECTED) { staJoining = false; staScanActive = false; return; }

  if (staJoining) {
    if (millis() - staJoinStart > STA_TIMEOUT_MS) {      // gave up on this one
      staJoining = false;
      WiFi.disconnect(false, false);
      staNextScan = millis() + 15000;
    }
    return;
  }

  if (staScanActive) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_RUNNING) return;
    staScanActive = false;
    staNextScan = millis() + 30000;                      // if nothing works, look again in 30 s
    if (n < 0) { staNextScan = millis() + 10000; return; }
    int best = -1, bestRssi = -1000;
    for (int i = 0; i < n; i++) {
      int k = findNet(WiFi.SSID(i));
      if (k >= 0 && WiFi.RSSI(i) > bestRssi) { best = k; bestRssi = WiFi.RSSI(i); }
    }
    if (best < 0) {                                      // hidden networks never show up in a scan
      for (int k = 0; k < netCount; k++) {
        if (nets[k].hidden) { best = k; break; }
      }
    }
    if (best >= 0) {
      WiFi.begin(nets[best].ssid.c_str(), nets[best].pass.c_str());
      staJoining = true;
      staJoinStart = millis();
    }
    return;
  }

  if (millis() >= staNextScan) {
    // A scan makes the radio hop channels, so don't do it while someone is using
    // the stick's own Wi-Fi (for example this web page).
    if (WiFi.softAPgetStationNum() > 0) { staNextScan = millis() + 10000; return; }
    WiFi.scanNetworks(true);
    staScanActive = true;
  }
}

// ---------------- USB MSC callbacks ----------------
static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (!usbMode) return -1;
  xSemaphoreTake(sdLock, portMAX_DELAY);
  bool ok = true;
  uint8_t* p = (uint8_t*)buffer;
  for (uint32_t i = 0; i < bufsize / 512 && ok; i++) {
    ok = hostRead(lba + i, p + i * 512);      // the car only sees its own part of the card
  }
  xSemaphoreGive(sdLock);
  return ok ? (int32_t)bufsize : -1;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (!usbMode) return -1;
  usbWriteCount = usbWriteCount + 1;   // only this callback writes it; loop() just reads
  xSemaphoreTake(sdLock, portMAX_DELAY);
  bool ok = true;
  for (uint32_t i = 0; i < bufsize / 512 && ok; i++) {
    ok = hostWrite(lba + i, buffer + i * 512);
  }
  xSemaphoreGive(sdLock);
  return ok ? (int32_t)bufsize : -1;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  // Host pressed "Eject" -> hand the card to Wi-Fi mode (done in loop()).
  if (load_eject && !start) ejectRequested = true;
  return true;
}

// ---------------- Free space in USB mode ----------------
// While the host owns the card we must not use FatFs, so we read the FAT
// table directly (read-only) and count unused clusters. This runs in small
// slices from loop(), only after the host has been idle for a few seconds.
static uint32_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool readSector(uint32_t lba, uint8_t* buf) {
  xSemaphoreTake(sdLock, portMAX_DELAY);
  bool ok = usbMode && hostRead(lba, buf);
  xSemaphoreGive(sdLock);
  return ok;
}

static bool looksLikeBootSector(const uint8_t* b) {
  return le16(b + 11) == 512 && b[13] != 0 && (b[16] == 1 || b[16] == 2);
}

void freeReset() {
  scanActive = false;
  freeState = 0;
}

void freeScanBegin() {
  scanWriteMark = usbWriteCount;
  uint8_t b[512];
  uint32_t part = 0;

  if (!readSector(0, b)) { lastWriteMs = millis(); return; }   // retry in a few seconds
  if (memcmp(b + 3, "EXFAT   ", 8) == 0) { freeState = 2; return; }

  if (!looksLikeBootSector(b)) {
    // Probably an MBR: find the first FAT partition.
    bool found = false;
    for (int i = 0; i < 4; i++) {
      const uint8_t* e = b + 446 + 16 * i;
      uint8_t t = e[4];
      if (t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E) {
        part = le32(e + 8);
        found = true;
        break;
      }
    }
    if (!found || part >= layout.hostSectors) { freeState = 2; return; }
    if (!readSector(part, b)) { lastWriteMs = millis(); return; }
    if (!looksLikeBootSector(b)) { freeState = 2; return; }
  }

  uint32_t spc = b[13], reserved = le16(b + 14), nfats = b[16], rootEnt = le16(b + 17);
  uint32_t tot = le16(b + 19);
  if (!tot) tot = le32(b + 32);
  uint32_t fatSz = le16(b + 22);
  if (!fatSz) fatSz = le32(b + 36);
  if (spc == 0 || fatSz == 0 || tot == 0) { freeState = 2; return; }

  uint32_t rootSectors = (rootEnt * 32 + 511) / 512;
  uint32_t meta = reserved + nfats * fatSz + rootSectors;
  if (tot <= meta) { freeState = 2; return; }
  uint32_t clusters = (tot - meta) / spc;
  if (clusters < 4085) { freeState = 2; return; }       // FAT12: not supported

  scanFat32 = clusters >= 65525;
  scanSpc = spc;
  scanClusters = clusters;
  scanFatStart = part + reserved;
  scanSectors = ((clusters + 2) * (scanFat32 ? 4 : 2) + 511) / 512;
  if (scanSectors > fatSz) scanSectors = fatSz;
  scanNext = 0;
  scanFree = 0;
  scanActive = true;
}

void freeScanStep() {
  uint8_t b[512];
  const uint32_t perSector = scanFat32 ? 128 : 256;
  for (int n = 0; n < 64 && scanNext < scanSectors; n++, scanNext++) {
    if (!readSector(scanFatStart + scanNext, b)) {
      scanActive = false;            // mode changed or read error: try again later
      lastWriteMs = millis();
      return;
    }
    uint32_t first = scanNext * perSector;
    for (uint32_t i = 0; i < perSector; i++) {
      uint32_t cl = first + i;
      if (cl < 2) continue;
      if (cl >= scanClusters + 2) break;
      uint32_t v = scanFat32 ? (le32(b + i * 4) & 0x0FFFFFFF) : le16(b + i * 2);
      if (v == 0) scanFree++;
    }
  }
  if (scanNext >= scanSectors) {
    freeBytes = (uint64_t)scanFree * scanSpc * 512;
    fsTotalBytes = (uint64_t)scanClusters * scanSpc * 512;
    freeState = 1;
    scanActive = false;
  }
}

void freeScanTick() {
  if (!usbMode || sectorCount == 0) return;
  if (usbWriteCount != lastCountSeen) {
    lastCountSeen = usbWriteCount;
    lastWriteMs = millis();
  }
  if (scanActive) { freeScanStep(); return; }
  if (freeState == 2) return;
  bool dirty = (freeState == 0) || (usbWriteCount != scanWriteMark);
  if (dirty && millis() - lastWriteMs > 3000) freeScanBegin();
}

// ---------------- Volume label ----------------
// The name Windows/macOS show for the drive is the FAT volume label stored ON the
// card (a field in the boot sector plus a special entry in the root directory).
// We set it directly on the card just before the drive is handed to the host:
// the card is mounted but idle and the host cannot see it yet.
// FAT16/FAT32 only (partitioned or not); anything else is left untouched.
static void setVolumeLabelRaw(const char* label) {
  uint8_t b[512], d[512], name[11];
  memset(name, ' ', sizeof(name));
  for (int i = 0; i < 11 && label[i]; i++) name[i] = (uint8_t)label[i];

  // Find the FAT boot sector (directly, or via the MBR partition table)
  uint32_t part = 0;
  if (!hostRead(0, b)) return;
  if (memcmp(b + 3, "EXFAT   ", 8) == 0) return;
  if (!looksLikeBootSector(b)) {
    bool found = false;
    for (int i = 0; i < 4; i++) {
      const uint8_t* e = b + 446 + 16 * i;
      uint8_t t = e[4];
      if (t == 0x04 || t == 0x06 || t == 0x0B || t == 0x0C || t == 0x0E) {
        part = le32(e + 8);
        found = true;
        break;
      }
    }
    if (!found || part >= layout.hostSectors) return;
    if (!hostRead(part, b)) return;
    if (!looksLikeBootSector(b)) return;
  }

  uint32_t spc = b[13], reserved = le16(b + 14), nfats = b[16], rootEnt = le16(b + 17);
  uint32_t tot = le16(b + 19);
  if (!tot) tot = le32(b + 32);
  uint32_t fatSz = le16(b + 22);
  if (!fatSz) fatSz = le32(b + 36);
  if (spc == 0 || fatSz == 0 || tot == 0) return;
  uint32_t rootSectors = (rootEnt * 32 + 511) / 512;
  uint32_t meta = reserved + nfats * fatSz + rootSectors;
  if (tot <= meta) return;
  uint32_t clusters = (tot - meta) / spc;
  if (clusters < 4085) return;                       // FAT12: not supported
  bool fat32 = clusters >= 65525;

  // 1) Label copy in the boot sector (only valid when the extended signature is 0x29)
  uint32_t sigOff = fat32 ? 66 : 38, lblOff = fat32 ? 71 : 43;
  if (b[sigOff] == 0x29 && memcmp(b + lblOff, name, 11) != 0) {
    memcpy(b + lblOff, name, 11);
    hostWrite(part, b);
  }

  // 2) Volume-label entry in the root directory (this is what Windows displays)
  uint32_t dirStart, dirSectors;
  uint32_t fatEnd = part + reserved + nfats * fatSz;
  if (fat32) {
    uint32_t rootClus = le32(b + 44);
    if (rootClus < 2) return;
    dirStart = fatEnd + (rootClus - 2) * spc;
    dirSectors = spc;                                // first cluster of the root directory
  } else {
    dirStart = fatEnd;
    dirSectors = rootSectors;
  }

  int freeSector = -1, freeIdx = -1;
  bool endOfDir = false;
  for (uint32_t sec = 0; sec < dirSectors && !endOfDir; sec++) {
    if (!hostRead(dirStart + sec, d)) return;
    for (int i = 0; i < 16; i++) {
      uint8_t* e = d + i * 32;
      if (e[0] == 0x00 || e[0] == 0xE5) {            // free slot
        if (freeSector < 0) { freeSector = (int)sec; freeIdx = i; }
        if (e[0] == 0x00) { endOfDir = true; break; }
        continue;
      }
      if ((e[11] & 0x3F) == 0x08) {                  // existing volume label entry
        if (memcmp(e, name, 11) != 0) {
          memcpy(e, name, 11);
          hostWrite(dirStart + sec, d);
        }
        return;
      }
    }
  }

  // No label entry yet: create one in the first free slot (if there is room)
  if (freeSector < 0) return;
  if (!hostRead(dirStart + freeSector, d)) return;
  uint8_t* e = d + freeIdx * 32;
  memset(e, 0, 32);
  memcpy(e, name, 11);
  e[11] = 0x08;                                      // attribute: volume label
  hostWrite(dirStart + freeSector, d);
}

// ---------------- Display ----------------
static const uint32_t C_BLACK = 0x000000, C_WHITE = 0xFFFFFF, C_CYAN = 0x00FFFF;
static const uint32_t C_GREEN = 0x00FF00, C_ORANGE = 0xFFA000, C_GREY = 0xB0B0B0;
String lastScreenKey;

// Short pop-up message on the screen (used by the BOOT-button lock chime)
String toastTitle, toastLine;
bool toastOk = true;
uint32_t toastUntil = 0;

void drawToast() {
  if (!lcdPtr) return;
  lcdPtr->fillScreen(C_BLACK);
  lcdPtr->setTextSize(2);
  lcdPtr->setTextColor(toastOk ? C_GREEN : C_ORANGE, C_BLACK);
  lcdPtr->setCursor(4, 3);
  lcdPtr->print(toastTitle.c_str());
  lcdPtr->setTextSize(1);
  lcdPtr->setTextColor(C_WHITE, C_BLACK);
  for (int i = 0; i < 4; i++) {                    // up to 4 lines of 25 characters
    String part = toastLine.substring(i * 25, (i + 1) * 25);
    if (part.length() == 0) break;
    lcdPtr->setCursor(4, 26 + i * 12);
    lcdPtr->print(part.c_str());
  }
}

void showToast(const String& title, const String& line, uint32_t ms, bool ok = true) {
  toastTitle = title;
  toastLine = line;
  toastOk = ok;
  toastUntil = millis() + ms;
  drawToast();
}

static String fmtBytes(uint64_t b) {
  char buf[20];
  if (b >= 1073741824ULL) snprintf(buf, sizeof(buf), "%.1f GB", b / 1073741824.0);
  else if (b >= 1048576ULL) snprintf(buf, sizeof(buf), "%.0f MB", b / 1048576.0);
  else snprintf(buf, sizeof(buf), "%.0f KB", b / 1024.0);
  return String(buf);
}

void displayInit() {
  if (!cfgDisplay) return;
  lcdPtr = new LGFX(cfgOffX, cfgOffY, cfgInvert, cfgRgb);
  lcdPtr->init();
  lcdPtr->setRotation(cfgRotation);
  lcdPtr->fillScreen(C_BLACK);
  pinMode(LCD_BL, OUTPUT);
  digitalWrite(LCD_BL, cfgBlOn);
  lcdPtr->setTextSize(2);
  lcdPtr->setTextColor(C_WHITE, C_BLACK);
  lcdPtr->setCursor(4, 22);
  lcdPtr->print("Starting...");
  lcdPtr->setTextSize(1);
  lcdPtr->setTextColor(C_GREY, C_BLACK);
  lcdPtr->setCursor(4, 50);
  lcdPtr->print("Version " FW_VERSION);
}

void displayUpdate() {
  if (!lcdPtr) return;
  static uint32_t last = 0;
  if (millis() - last < 300) return;
  last = millis();

  if (millis() < toastUntil) return;                 // a pop-up message is showing
  if (toastUntil != 0) { toastUntil = 0; lastScreenKey = ""; }   // it expired: redraw normal screen

  String title, label, value, extra;
  uint32_t titleColor;
  bool bigIp = false;                    // client mode: show the IP as large as possible

  if (usbMode) {
    title = "USB Drive";
    titleColor = C_ORANGE;
    label = "Free space";
    if (sectorCount == 0) { value = "No SD card"; }
    else if (freeState == 1) { value = fmtBytes(freeBytes); extra = "of " + fmtBytes(fsTotalBytes); }
    else if (freeState == 2) { value = "n/a"; extra = "FAT16/32 only"; }
    else { value = "..."; extra = "calculating"; }
  } else if (WiFi.status() == WL_CONNECTED) {
    title = "Wi-Fi Client";
    titleColor = C_GREEN;
    value = WiFi.localIP().toString();
    bigIp = true;
  } else {
    title = "Wi-Fi AP";
    titleColor = C_CYAN;
    label = "IP address";
    value = WiFi.softAPIP().toString();
    extra = "Clients: " + String(WiFi.softAPgetStationNum());
  }

  String key = title + "|" + value + "|" + extra;
  if (key == lastScreenKey) return;      // nothing changed: no redraw, no flicker
  lastScreenKey = key;

  lcdPtr->fillScreen(C_BLACK);
  lcdPtr->setTextSize(2);
  lcdPtr->setTextColor(titleColor, C_BLACK);
  lcdPtr->setCursor(4, 3);
  lcdPtr->print(title.c_str());

  if (bigIp) {
    // Single-line IP, scaled up to use the full screen width (max 3x).
    // Each character is 6 px wide at 1x; keep a 4 px margin on both sides.
    float scale = (float)(lcdPtr->width() - 8) / (6.0f * value.length());
    if (scale > 3.0f) scale = 3.0f;
    int textH = (int)(8 * scale);
    int y = 22 + (58 - textH) / 2;          // centred in the area below the title
    lcdPtr->setTextSize(scale);
    lcdPtr->setTextColor(C_WHITE, C_BLACK);
    lcdPtr->setCursor(4, y);
    lcdPtr->print(value.c_str());
  } else {
    lcdPtr->setTextSize(1);
    lcdPtr->setTextColor(C_GREY, C_BLACK);
    lcdPtr->setCursor(4, 26);
    lcdPtr->print(label.c_str());

    lcdPtr->setTextSize(value.length() <= 13 ? 2 : 1);   // long IPs drop to a smaller font
    lcdPtr->setTextColor(C_WHITE, C_BLACK);
    lcdPtr->setCursor(4, 37);
    lcdPtr->print(value.c_str());
  }

  if (extra.length()) {
    lcdPtr->setTextSize(extra.length() <= 13 ? 2 : 1);
    lcdPtr->setCursor(4, 60);
    lcdPtr->print(extra.c_str());
  }
}

void formatProgress(int percent) {
  static int last = -1;
  if (percent == last) return;
  last = percent;
  showToast("Card setup", String(percent) + "% done. Do not unplug the stick.", 600000, true);
}

// ---------------- Mode switching ----------------
void setUsbMode(bool on) {
  if (on == usbMode) return;
  if (on) {
    if (uploadFile) uploadFile.close();
    freeReset();                  // card may have changed in Wi-Fi mode: recount
    staDisconnect();              // router link is only used in Wi-Fi mode
    xSemaphoreTake(sdLock, portMAX_DELAY);
    SD_MMC.end();                 // flush and drop cached FAT state
    mountSD();
    if (sectorCount > 0) setVolumeLabelRaw(cfgLabel.c_str());   // name the drive
    usbMode = true;
    xSemaphoreGive(sdLock);
    msc.mediaPresent(true);       // host sees the drive (re)appear
  } else {
    msc.mediaPresent(false);      // host sees the drive removed
    delay(600);                   // give the host time to notice
    xSemaphoreTake(sdLock, portMAX_DELAY);
    usbMode = false;
    SD_MMC.end();                 // remount fresh so we see the host's changes
    mountSD();
    xSemaphoreGive(sdLock);
    staConnectSaved();            // join the saved router, if any
  }
}

// ---------------- Web helpers ----------------
String jsonEscape(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c < 0x20) { o += ' '; }
    else o += c;
  }
  return o;
}

// Normalise a user-supplied path; rejects "..".
String safePath(String p) {
  if (p.length() == 0) p = "/";
  if (p[0] != '/') p = "/" + p;
  if (p.indexOf("..") >= 0) return "/";
  while (p.length() > 1 && p.endsWith("/")) p.remove(p.length() - 1);
  return p;
}

bool requireWifiMode() {
  if (usbMode) {
    server.send(423, "text/plain", "Drive is in USB mode. Switch to Wi-Fi mode first.");
    return false;
  }
  return true;
}

// ---------------- Web handlers ----------------
const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wi-Fi USB Stick</title>
<style>
 body{font:16px system-ui,sans-serif;margin:0 auto;padding:12px;max-width:720px}
 #bar{padding:10px;border-radius:8px;background:#eef;margin-bottom:10px}
 #bar.usb{background:#fed}
 button{padding:6px 10px;margin:2px;font-size:14px}
 table{width:100%;border-collapse:collapse}
 td{padding:6px 4px;border-bottom:1px solid #ddd;word-break:break-all}
 td.s{white-space:nowrap;color:#666;font-size:13px}
 td.a{text-align:right}
 a{color:#06c;text-decoration:none;cursor:pointer}
 #path{font-family:monospace;margin:8px 0}
 #msg{color:#b00;min-height:1.2em}
 details{background:#f4f4f4;border-radius:8px;padding:8px 10px;margin-bottom:10px}
 summary{cursor:pointer;font-weight:600}
 details input[type=text],details input[type=password],details select{width:100%;box-sizing:border-box;padding:7px;margin:4px 0;font-size:15px}
 #wst{margin:6px 0;font-size:14px}
 .nrow{display:flex;justify-content:space-between;align-items:center;padding:4px 0;border-bottom:1px solid #ddd;font-size:14px}
 #tesla ul{margin:6px 0 6px 18px;padding:0}
 #tesla li{margin:4px 0}
 #tesla h4{margin:12px 0 4px}
 #tesla .sm,#card .sm{font-size:13px;color:#666}
 #card select{margin:4px 0}
</style></head><body>
<h2>Wi-Fi USB Stick <small id="ver" style="font-size:13px;font-weight:400;color:#888"></small></h2>
<div id="bar"><span id="mode"></span><br><button id="toggle"></button></div>
<details id="wifi"><summary>Wi-Fi network (join your router)</summary>
 <div id="wst"></div>
 <div id="wsaved"></div>
 <button id="scan">Scan for networks</button>
 <select id="nets"><option value="">(scan to list networks)</option></select>
 <input type="text" id="ssid" placeholder="Network name (type it here if not listed)" maxlength="32" autocapitalize="off" autocomplete="off">
 <input type="password" id="pass" placeholder="Password (blank if open)" maxlength="63" autocomplete="off">
 <label><input type="checkbox" id="hid"> Hidden / not listed (skip the check)</label>
 <div><button id="wconn">Connect</button></div>
</details>
<details id="card"><summary>SD card setup</summary>
 <div id="cardinfo"></div>
 <p class="sm">Splitting the card gives the car a small "Boombox" drive and keeps a separate "CHIME_STORE" area for the stick that the car never sees. <b>It erases everything on the card.</b> Large cards can take several minutes; the stick restarts when done.</p>
 <select id="cardsize">
  <option value="0">Boombox 4 GB (recommended)</option>
  <option value="1">Boombox 8 GB</option>
  <option value="2">Half the card</option>
 </select>
 <button id="cardsplit">Erase and split the card</button>
</details>
<details id="tesla"><summary>Tesla lock chime guide</summary>
 <h4>What the car needs</h4>
 <ul>
  <li><b>File name:</b> exactly <code>LockChime.wav</code> (capital L and C). Watch for <code>LockChime.wav.wav</code>.</li>
  <li><b>Format:</b> a real WAV file (PCM). An MP3 renamed to .wav will not work. Safe choice: 16-bit PCM, 44.1 kHz, mono or stereo.</li>
  <li><b>Size:</b> under 1 MB (about 5 seconds of 16-bit 44.1 kHz stereo). Short sounds (1-3 s) work best.</li>
  <li><b>Location:</b> the root of the drive, not in a folder. (A few sites say <code>/Boombox/</code> instead; if the car ignores the root, try that.)</li>
  <li><b>One sound only:</b> the car uses a single file named LockChime.wav.</li>
  <li><b>Drive format:</b> FAT32 or exFAT (not NTFS). FAT32 is best with this stick.</li>
  <li><b>Car:</b> needs the Lock Sound feature (added in software 2023.44) and the external pedestrian speaker. Use a USB port that carries data (the glovebox port on newer cars).</li>
  <li><b>Select it in the car:</b> open the Lock Sound setting and choose the USB/custom sound (on many versions: Toybox, Boombox, Lock Sound, USB).</li>
 </ul>
 <h4>Changing the chime with this stick</h4>
 <ul>
  <li>Upload .wav files here (any folder). Press <b>Play</b> to listen first.</li>
  <li>Press <b>Make Lock Chime</b> on the one you want. It is copied to /LockChime.wav and the original stays where it is. The page warns you if the file doesn't look right.</li>
  <li>Press <b>Give drive back to USB host</b> at the top so the car sees the drive again.</li>
  <li>The BOOT button on the stick picks a random 16-bit PCM .wav (under the size limit) and makes it the lock chime.</li>
 </ul>
 <h4>If the car still plays the old sound</h4>
 <ul>
  <li>Re-open the Lock Sound setting and select USB again. Some owners report the car keeps the old file until they pick another sound and then USB again.</li>
  <li>Check the name, the size, that it is a real PCM WAV, and that it is at the root of the drive.</li>
  <li>Try the other USB port, and make sure Lock Sound is switched on.</li>
 </ul>
 <p class="sm">Do not use this stick as the car's dashcam or Sentry drive: its USB is full speed only, and if the car offers to format the drive, say no (that would erase the card). Software and models differ, and reports from owners don't always agree, so your owner's manual is the final word.</p>
</details>
<div id="tools">
 <input type="file" id="up" multiple>
 <button id="mk">New folder</button>
</div>
<div id="path"></div>
<div id="msg"></div>
<table id="files"></table>
<script>
let dir='/',usb=false;
const $=s=>document.querySelector(s);
const enc=encodeURIComponent;
const join=(d,n)=>(d==='/'?'':d)+'/'+n;
const msg=t=>{$('#msg').textContent=t||''};
async function api(u,o){const r=await fetch(u,o);if(!r.ok)throw new Error(await r.text());return r}
function fmt(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':(n/1048576).toFixed(1)+' MB'}
function btn(label,fn){const b=document.createElement('button');b.textContent=label;b.onclick=fn;return b}

async function load(){
 stopPlay();
 msg();
 wstatus();
 const st=await (await fetch('/api/status')).json();
 usb=!!st.usb;
 $('#bar').className=usb?'usb':'';
 $('#mode').textContent=usb
  ?'USB mode: the host owns the drive. Editing is disabled.'
  :'Wi-Fi mode: you can edit files. The host sees the drive as ejected. '+(st.total?fmt(st.used)+' used of '+fmt(st.total):'');
 cardinfo(st);
 if(st.ver)$('#ver').textContent='v'+st.ver;
 $('#toggle').textContent=usb?'Switch to Wi-Fi mode (edit files)':'Give drive back to USB host';
 $('#tools').style.display=usb?'none':'';
 $('#path').textContent=usb?'':dir;
 const t=$('#files');t.innerHTML='';
 if(usb)return;
 let list;
 try{list=await (await api('/api/list?dir='+enc(dir))).json()}catch(e){msg(e.message);return}
 list.sort((a,b)=>b.d-a.d||a.n.localeCompare(b.n));
 if(dir!=='/'){
  const r=t.insertRow();const c=r.insertCell();c.colSpan=3;
  const a=document.createElement('a');a.textContent='.. (up)';
  a.onclick=()=>{dir=dir.substring(0,dir.lastIndexOf('/'))||'/';load()};
  c.appendChild(a);
 }
 for(const f of list){
  const r=t.insertRow();
  const c=r.insertCell();const a=document.createElement('a');
  if(f.d){a.textContent='[ '+f.n+' ]';a.onclick=()=>{dir=join(dir,f.n);load()}}
  else{a.textContent=f.n;a.href='/download?path='+enc(join(dir,f.n))}
  c.appendChild(a);
  const s=r.insertCell();s.className='s';s.textContent=f.d?'':fmt(f.s);
  const x=r.insertCell();x.className='a';
  if(!f.d&&/\.wav$/i.test(f.n)){
   x.appendChild(playBtn(join(dir,f.n)));
   if(!(dir==='/'&&f.n.toLowerCase()==='lockchime.wav'))
   x.appendChild(btn('Make Lock Chime',async()=>{
    let m='Copy '+f.n+' to /LockChime.wav?\n(An existing LockChime.wav will be replaced.)';
    try{
     const i=await (await api('/api/wavinfo?path='+enc(join(dir,f.n)))).json();
     if(i.ok)m+='\n\n'+(i.ch==1?'Mono':i.ch==2?'Stereo':i.ch+' channels')+', '+i.bits+'-bit, '+i.rate+' Hz, '+i.secs.toFixed(1)+' s, '+fmt(i.size);
     if(i.warn.length)m+='\n\nWarnings:\n- '+i.warn.join('\n- ')+'\n\nThe car may not play this file. Copy it anyway?';
    }catch(e){}
    if(!confirm(m))return;
    msg('Copying...');
    try{await api('/lockchime?path='+enc(join(dir,f.n)),{method:'POST'});msg();alert('LockChime.wav created at the root of the SD card.');load()}catch(e){msg(e.message)}
   }));
  }
  x.appendChild(btn('Rename',async()=>{
   const n=prompt('New name',f.n);
   if(!n||n===f.n)return;
   try{await api('/rename?from='+enc(join(dir,f.n))+'&to='+enc(join(dir,n)),{method:'POST'});load()}catch(e){msg(e.message)}
  }));
  x.appendChild(btn('Delete',async()=>{
   if(!confirm('Delete '+f.n+'?'))return;
   try{await api('/delete?path='+enc(join(dir,f.n)),{method:'POST'});load()}catch(e){msg(e.message)}
  }));
 }
}

$('#toggle').onclick=async()=>{
 msg('Switching...');
 try{await api('/mode?usb='+(usb?0:1),{method:'POST'})}catch(e){msg(e.message)}
 load();
};
$('#mk').onclick=async()=>{
 const n=prompt('Folder name');if(!n)return;
 try{await api('/mkdir?path='+enc(join(dir,n)),{method:'POST'});load()}catch(e){msg(e.message)}
};
$('#up').onchange=async e=>{
 for(const f of e.target.files){
  msg('Uploading '+f.name+'...');
  const fd=new FormData();fd.append('file',f,f.name);
  try{await api('/upload?dir='+enc(dir),{method:'POST',body:fd})}catch(err){msg(err.message);break}
 }
 e.target.value='';load();
};

let wpoll=null;
async function wstatus(){
 try{
  const s=await (await fetch('/api/wifi/status')).json();
  let t=s.connected?('Connected to '+s.ssid+' ('+s.rssi+' dBm). Open http://'+s.ip):'Not connected to a network.';
  if(s.connecting)t='Connecting...';
  else if(s.result==='failed'&&!s.connected)t+=' Last attempt failed (wrong password or out of range).';
  $('#wst').textContent=t;
  if(!s.connecting)wsaved();
  if(!s.connecting&&wpoll){clearInterval(wpoll);wpoll=null}
 }catch(e){$('#wst').textContent='Reconnecting to the stick...'}
}
function fillNets(list){
 const best={};
 for(const n of list){if(!best[n.s]||n.r>best[n.s].r)best[n.s]=n}
 const arr=Object.values(best).sort((a,b)=>b.r-a.r);
 const sel=$('#nets');sel.innerHTML='';
 const o0=document.createElement('option');
 o0.value='';o0.textContent=arr.length?'Choose a network...':'No networks found';
 sel.appendChild(o0);
 for(const n of arr){
  const o=document.createElement('option');
  o.value=n.s;o.textContent=n.s+(n.o?' (open)':'')+'  '+n.r+' dBm';
  sel.appendChild(o);
 }
}
$('#nets').onchange=e=>{if(e.target.value)$('#ssid').value=e.target.value};
$('#scan').onclick=async()=>{
 $('#wst').textContent='Scanning...';
 try{await api('/wifi/scan',{method:'POST'})}catch(e){}
 for(let i=0;i<20;i++){
  await new Promise(r=>setTimeout(r,1000));
  try{
   const r=await (await fetch('/api/wifi/networks')).json();
   if(r.scanning)continue;
   fillNets(r.list);wstatus();return;
  }catch(e){}
 }
 $('#wst').textContent='Scan did not finish. Try again.';
};
async function wconnect(force){
 $('#wst').textContent='Checking network...';
 const body=new URLSearchParams({ssid:$('#ssid').value,pass:$('#pass').value,hidden:force?'1':'0'});
 const r=await fetch('/wifi/connect',{method:'POST',body});
 const t=await r.text();
 if(r.status===404&&!force){
  if(confirm('"'+$('#ssid').value+'" was not found in a scan (it may be hidden or out of range). Try connecting anyway?'))return wconnect(true);
 }
 $('#wst').textContent=t;
 if(r.ok&&!wpoll)wpoll=setInterval(wstatus,2000);
}
$('#wconn').onclick=()=>wconnect($('#hid').checked).catch(e=>{$('#wst').textContent=e.message});

async function wsaved(){
 try{
  const r=await (await fetch('/api/wifi/saved')).json();
  const box=$('#wsaved');box.innerHTML='';
  if(!r.list.length){box.textContent='No saved networks yet.';return}
  const h=document.createElement('div');h.textContent='Saved networks:';h.style.fontWeight='600';box.appendChild(h);
  for(const n of r.list){
   const row=document.createElement('div');row.className='nrow';
   const t=document.createElement('span');
   t.textContent=n.s+(n.c?' (connected)':'')+(n.h?' (hidden)':'');
   row.appendChild(t);
   row.appendChild(btn('Delete',async()=>{
    if(!confirm('Delete the saved network "'+n.s+'"?'))return;
    try{await api('/wifi/delete',{method:'POST',body:new URLSearchParams({ssid:n.s})})}catch(e){$('#wst').textContent=e.message}
    wsaved();wstatus();
   }));
   box.appendChild(row);
  }
 }catch(e){}
}

let player=null,playBtnEl=null;
function stopPlay(){
 if(player){player.pause();player=null}
 if(playBtnEl){playBtnEl.textContent='Play';playBtnEl=null}
}
function playBtn(path){
 const b=btn('Play',()=>{
  if(playBtnEl===b){stopPlay();return}
  stopPlay();
  player=new Audio('/play?path='+enc(path));
  playBtnEl=b;b.textContent='Stop';
  player.onended=stopPlay;
  player.onerror=()=>{stopPlay();msg('Could not play this file. Is it a valid WAV?')};
  player.play().catch(()=>{stopPlay()});
 });
 return b;
}

function gb(mb){return (mb/1024).toFixed(mb<10240?1:0)+' GB'}
function cardinfo(st){
 const c=$('#cardinfo');
 if(!st.cardMB){c.textContent='No SD card found.';return}
 c.textContent=st.split
  ?'This card is split: Boombox '+gb(st.bbMB)+' (what the car sees) + storage '+gb(st.stMB)+'.'
  :'This card is not split: the car sees all '+gb(st.cardMB)+'.';
}
$('#cardsplit').onclick=async()=>{
 if(usb){alert('Switch to Wi-Fi mode first (button at the top).');return}
 const size=$('#cardsize').value;
 const a=prompt('This ERASES EVERYTHING on the SD card, including any recordings and sounds.\n\nType ERASE to continue.');
 if(a!=='ERASE')return;
 try{
  await api('/card/split',{method:'POST',body:new URLSearchParams({size,confirm:'ERASE'})});
  msg('Setting up the card. This can take a few minutes on large cards. The stick restarts when it is done; then reconnect to its Wi-Fi and reload this page.');
 }catch(e){msg(e.message)}
};

load();
</script></body></html>)HTML";

void handleRoot() {
  server.send_P(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  String out = "{\"usb\":";
  out += usbMode ? "1" : "0";
  if (!usbMode) {
    out += ",\"total\":" + String((uint32_t)(cardTotal));
    out += ",\"used\":" + String((uint32_t)(SD_MMC.usedBytes()));
  }
  out += ",\"ver\":\"" FW_VERSION "\"";
  out += ",\"cardMB\":" + String((unsigned long)(layout.total / 2048));
  out += ",\"split\":" + String(layout.split ? 1 : 0);
  if (layout.split) {
    out += ",\"bbMB\":" + String((unsigned long)(layout.hostSectors / 2048));
    out += ",\"stMB\":" + String((unsigned long)(layout.stSize / 2048));
  }
  out += "}";
  server.send(200, "application/json", out);
}

// Erase the card and split it (the actual work runs from loop(), then the stick restarts).
int splitChoice = -1;
void handleCardSplit() {
  if (!requireWifiMode()) return;
  if (layout.total == 0) { server.send(400, "text/plain", "No SD card found."); return; }
  if (server.arg("confirm") != "ERASE") { server.send(400, "text/plain", "Not confirmed."); return; }
  int c = server.arg("size").toInt();
  if (c < 0 || c > 2) c = 0;
  splitChoice = c;
  server.send(200, "text/plain", "OK");
}

void doCardSplit() {
  int choice = splitChoice;
  splitChoice = -1;
  if (uploadFile) uploadFile.close();
  staDisconnect();
  xSemaphoreTake(sdLock, portMAX_DELAY);
  formatting = true;
  const char* err = formatSplitCard(layout.total, choice, esp_random());
  formatting = false;
  xSemaphoreGive(sdLock);
  if (err[0]) {
    showToast("Card setup failed", err, 15000, false);
    delay(15000);
  } else {
    showToast("Card ready", "Restarting...", 3000, true);
    delay(2000);
  }
  ESP.restart();                       // start fresh with the new layout
}

void handleList() {
  if (!requireWifiMode()) return;
  String dir = safePath(server.arg("dir"));
  File root = SD_MMC.open(dir);
  if (!root || !root.isDirectory()) {
    server.send(404, "text/plain", "Folder not found");
    return;
  }
  String out = "[";
  bool first = true;
  File f = root.openNextFile();
  while (f) {
    if (!first) out += ',';
    first = false;
    out += "{\"n\":\"" + jsonEscape(String(f.name())) + "\",\"d\":";
    out += f.isDirectory() ? "1" : "0";
    out += ",\"s\":" + String((uint32_t)f.size()) + "}";
    f = root.openNextFile();
  }
  out += "]";
  server.send(200, "application/json", out);
}

void handleDownload() {
  if (!requireWifiMode()) return;
  String path = safePath(server.arg("path"));
  File f = SD_MMC.open(path);
  if (!f || f.isDirectory()) {
    server.send(404, "text/plain", "Not found");
    return;
  }
  String name = path.substring(path.lastIndexOf('/') + 1);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + name + "\"");
  server.streamFile(f, "application/octet-stream");
  f.close();
}

void handleUploadData() {
  if (usbMode) return;
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    String dir = safePath(server.arg("dir"));
    String name = up.filename;
    name.replace("/", "_");
    name.replace("\\", "_");
    String path = (dir == "/" ? "" : dir) + "/" + name;
    if (uploadFile) uploadFile.close();
    uploadFile = SD_MMC.open(path, FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
  }
}

void handleUploadDone() {
  if (usbMode) server.send(423, "text/plain", "Drive is in USB mode.");
  else server.send(200, "text/plain", "OK");
}

void handleRename() {
  if (!requireWifiMode()) return;
  String from = safePath(server.arg("from"));
  String to = safePath(server.arg("to"));
  if (from == "/" || to == "/") { server.send(400, "text/plain", "Bad name"); return; }
  if (SD_MMC.exists(to)) { server.send(409, "text/plain", "A file with that name already exists"); return; }
  if (SD_MMC.rename(from, to)) server.send(200, "text/plain", "OK");
  else server.send(500, "text/plain", "Rename failed");
}

void handleDelete() {
  if (!requireWifiMode()) return;
  String path = safePath(server.arg("path"));
  if (path == "/") { server.send(400, "text/plain", "Bad path"); return; }
  File f = SD_MMC.open(path);
  bool isDir = f && f.isDirectory();
  if (f) f.close();
  bool ok = isDir ? SD_MMC.rmdir(path) : SD_MMC.remove(path);
  if (ok) server.send(200, "text/plain", "OK");
  else server.send(500, "text/plain", isDir ? "Folder must be empty" : "Delete failed");
}

void handleMkdir() {
  if (!requireWifiMode()) return;
  String path = safePath(server.arg("path"));
  if (SD_MMC.mkdir(path)) server.send(200, "text/plain", "OK");
  else server.send(500, "text/plain", "Could not create folder");
}

// ---- WAV header reading (used to warn about files the car may not play) ----
struct WavInfo {
  uint16_t fmt, ch, bits;
  uint32_t rate, dataBytes, fileBytes;
};

// Reads the RIFF/WAVE header. Returns false if this is not a WAV file at all.
static bool readWavInfo(File& f, WavInfo& w) {
  memset(&w, 0, sizeof(w));
  w.fileBytes = (uint32_t)f.size();
  uint8_t h[12];
  f.seek(0);
  if (f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) return false;
  uint32_t pos = 12;
  bool gotFmt = false;
  while (pos + 8 <= w.fileBytes) {
    uint8_t c[8];
    f.seek(pos);
    if (f.read(c, 8) != 8) break;
    uint32_t sz = le32(c + 4);
    if (memcmp(c, "fmt ", 4) == 0 && sz >= 16) {
      uint8_t b[16];
      if (f.read(b, 16) != 16) break;
      w.fmt = le16(b);
      w.ch = le16(b + 2);
      w.rate = le32(b + 4);
      w.bits = le16(b + 14);
      gotFmt = true;
    } else if (memcmp(c, "data", 4) == 0) {
      w.dataBytes = sz;
      break;
    }
    if (sz > 0x7FFFFFFFUL - pos - 16) break;             // corrupt chunk size
    pos += 8 + sz + (sz & 1);
  }
  return gotFmt;
}

static float wavSeconds(const WavInfo& w) {
  if (w.rate == 0 || w.ch == 0 || w.bits == 0) return 0;
  uint32_t bytes = w.dataBytes;
  if (bytes == 0 || bytes > w.fileBytes) bytes = (w.fileBytes > 44) ? (w.fileBytes - 44) : 0;
  return (float)bytes / ((float)w.rate * w.ch * (w.bits / 8.0f));
}

// JSON string-array body (without the brackets) listing why a file might not work as a lock chime.
static String wavWarnings(bool ok, const WavInfo& w) {
  String out;
  auto add = [&](const String& t) {
    if (out.length()) out += ',';
    out += "\"" + t + "\"";
  };
  if (!ok) {
    add("Not a valid WAV file (an MP3 renamed to .wav will not work)");
    return out;
  }
  if (w.fmt != 1) add("Not plain PCM audio. Re-export it as 16-bit PCM WAV");
  if (w.bits != 16) add("It is " + String((unsigned int)w.bits) + "-bit. 16-bit PCM is the safe choice");
  if (w.ch > 2) add("More than 2 channels. Use mono or stereo");
  if (w.fileBytes >= 1000000UL) add("File is " + String((unsigned long)(w.fileBytes / 1000)) + " KB. Tesla needs it under 1 MB");
  float secs = wavSeconds(w);
  if (secs > 10.0f) add("Long sound (" + String(secs, 0) + " s). Lock chimes are usually 1 to 5 seconds");
  return out;
}

// Copy a .wav to /LockChime.wav at the card root. The original is left untouched.
// The copy goes to a temp file first so a failed copy never destroys an existing chime.
// Returns "" on success, otherwise a message describing what went wrong.
String makeLockChime(const String& src) {
  String lower = src;
  lower.toLowerCase();
  if (!lower.endsWith(".wav")) return "Only .wav files can be used as a lock chime.";
  if (lower == "/lockchime.wav") return "That file is already the lock chime.";
  File in = SD_MMC.open(src, FILE_READ);
  if (!in || in.isDirectory()) return "File not found.";

  const char* tmpPath = "/LockChime.tmp";
  const char* dstPath = "/LockChime.wav";
  File out = SD_MMC.open(tmpPath, FILE_WRITE);
  if (!out) {
    in.close();
    return "Could not create the copy.";
  }
  const size_t BUF_SIZE = 4096;
  uint8_t* buf = (uint8_t*)malloc(BUF_SIZE);
  if (!buf) {
    in.close();
    out.close();
    SD_MMC.remove(tmpPath);
    return "Out of memory.";
  }
  bool ok = true;
  while (in.available()) {
    size_t n = in.read(buf, BUF_SIZE);
    if (n == 0) break;
    if (out.write(buf, n) != n) { ok = false; break; }
  }
  free(buf);
  in.close();
  out.close();
  if (!ok) {
    SD_MMC.remove(tmpPath);
    return "Copy failed (is the card full?).";
  }
  if (SD_MMC.exists(dstPath)) SD_MMC.remove(dstPath);
  if (!SD_MMC.rename(tmpPath, dstPath)) return "Could not rename the copy to LockChime.wav.";
  return "";
}

void handleLockChime() {
  if (!requireWifiMode()) return;
  String err = makeLockChime(safePath(server.arg("path")));
  if (err.length() == 0) { server.send(200, "text/plain", "OK"); return; }
  int code = 500;
  if (err == "File not found.") code = 404;
  else if (err.startsWith("Only") || err.startsWith("That")) code = 400;
  server.send(code, "text/plain", err);
}

// Stream a file to the browser for listening (the web page's Play button).
void handlePlay() {
  if (!requireWifiMode()) return;
  String path = safePath(server.arg("path"));
  File f = SD_MMC.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    server.send(404, "text/plain", "File not found.");
    return;
  }
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, "audio/wav");
  f.close();
}

// Describe a WAV file (format, length) and list anything the car may not like.
void handleWavInfo() {
  if (!requireWifiMode()) return;
  String path = safePath(server.arg("path"));
  File f = SD_MMC.open(path, FILE_READ);
  if (!f || f.isDirectory()) {
    server.send(404, "text/plain", "File not found.");
    return;
  }
  WavInfo w;
  bool ok = readWavInfo(f, w);
  f.close();
  String out = "{\"ok\":" + String(ok ? 1 : 0);
  out += ",\"size\":" + String((unsigned long)w.fileBytes);
  out += ",\"fmt\":" + String((unsigned int)w.fmt);
  out += ",\"ch\":" + String((unsigned int)w.ch);
  out += ",\"rate\":" + String((unsigned long)w.rate);
  out += ",\"bits\":" + String((unsigned int)w.bits);
  out += ",\"secs\":" + String(wavSeconds(w), 2);
  out += ",\"warn\":[" + wavWarnings(ok, w) + "]}";
  server.send(200, "application/json", out);
}

// ---- Wi-Fi client handlers ----
void handleWifiScan() {
  if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) WiFi.scanNetworks(true);   // async
  server.send(200, "text/plain", "OK");
}

void handleWifiNetworks() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) {
    server.send(200, "application/json", "{\"scanning\":1,\"list\":[]}");
    return;
  }
  String out = "{\"scanning\":0,\"list\":[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.length() == 0) continue;   // hidden network
    if (!first) out += ',';
    first = false;
    out += "{\"s\":\"" + jsonEscape(s) + "\",\"r\":" + String(WiFi.RSSI(i));
    out += ",\"o\":";
    out += (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "1" : "0";
    out += "}";
  }
  out += "]}";
  server.send(200, "application/json", out);
}

void handleWifiStatus() {
  bool conn = (WiFi.status() == WL_CONNECTED) && !usbMode;
  String out = "{\"connected\":" + String(conn ? 1 : 0);
  if (conn) {
    out += ",\"ssid\":\"" + jsonEscape(WiFi.SSID()) + "\"";
    out += ",\"ip\":\"" + WiFi.localIP().toString() + "\"";
    out += ",\"rssi\":" + String(WiFi.RSSI());
  }
  out += ",\"saved\":" + String(netCount);
  out += ",\"connecting\":" + String(staPending ? 1 : 0);
  out += ",\"result\":\"" + staResult + "\"}";
  server.send(200, "application/json", out);
}

// The remembered networks (names only - passwords are never sent to the browser).
void handleWifiSaved() {
  bool conn = (WiFi.status() == WL_CONNECTED) && !usbMode;
  String cur = conn ? WiFi.SSID() : String("");
  String out = "{\"list\":[";
  for (int i = 0; i < netCount; i++) {
    if (i) out += ',';
    out += "{\"s\":\"" + jsonEscape(nets[i].ssid) + "\",\"h\":" + String(nets[i].hidden ? 1 : 0);
    out += ",\"c\":" + String((conn && nets[i].ssid == cur) ? 1 : 0) + "}";
  }
  out += "]}";
  server.send(200, "application/json", out);
}

void handleWifiConnect() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  bool hidden = server.arg("hidden") == "1";

  if (ssid.length() < 1 || ssid.length() > 32) {
    server.send(400, "text/plain", "Network name must be 1-32 characters.");
    return;
  }
  if (pass.length() != 0 && (pass.length() < 8 || pass.length() > 63)) {
    server.send(400, "text/plain", "Password must be 8-63 characters (leave blank for an open network).");
    return;
  }
  if (findNet(ssid) < 0 && netCount >= MAX_NETS) {
    server.send(409, "text/plain", "The saved list is full (" + String(MAX_NETS) + " networks). Delete one first.");
    return;
  }

  // Make sure the network exists (skipped for hidden networks).
  if (!hidden) {
    uint32_t t0 = millis();
    while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - t0 < 8000) delay(100);
    int n = WiFi.scanNetworks();   // blocking scan
    bool found = false;
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == ssid) { found = true; break; }
    }
    WiFi.scanDelete();
    if (!found) {
      server.send(404, "text/plain", "Network not found in the scan.");
      return;
    }
  }

  // In USB mode just remember it; it is joined when Wi-Fi mode is selected.
  if (usbMode) {
    addOrUpdateNet(ssid, pass, hidden);
    server.send(200, "text/plain", "Saved. The stick will connect when you switch to Wi-Fi mode.");
    return;
  }

  // Connect in the background. Note: joining a router can move the stick's own
  // access point to the router's channel, so your phone may briefly drop and
  // rejoin the stick's Wi-Fi. The network is only remembered once it connects.
  pendingSsid = ssid;
  pendingPass = pass;
  pendingHidden = hidden;
  staPending = true;
  staResult = "";
  staStart = millis();
  WiFi.disconnect(false, false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  server.send(200, "text/plain", "Connecting...");
}

void handleWifiDelete() {
  String ssid = server.arg("ssid");
  bool wasCurrent = !usbMode && WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid;
  if (!deleteNet(ssid)) {
    server.send(404, "text/plain", "That network is not in the saved list.");
    return;
  }
  if (wasCurrent) {            // leave it, then try the remaining remembered networks
    staDisconnect();
    staConnectSaved();
  }
  server.send(200, "text/plain", "OK");
}

void handleMode() {
  setUsbMode(server.arg("usb") == "1");
  server.send(200, "text/plain", "OK");
}

// ---------------- Random lock chime (BOOT button) ----------------
String lastChime;   // the file picked last time, so a press tries to choose a different one

// Walk the card and pick one .wav at random (reservoir sampling: every candidate
// has the same chance, and no list of paths needs to be kept in memory).
// A file the car should be able to play: a real PCM WAV, 16-bit, mono or stereo.
static bool isGoodChimeWav(File& f) {
  WavInfo w;
  return readWavInfo(f, w) && w.fmt == 1 && w.bits == 16 && w.ch >= 1 && w.ch <= 2;
}

static void pickRandomWav(const String& dirPath, int depth, const String& skip, String& chosen, uint32_t& count) {
  File dir = SD_MMC.open(dirPath);
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f) {
    String name = String(f.name());
    String full = (dirPath == "/" ? "" : dirPath) + "/" + name;
    if (f.isDirectory()) {
      if (depth < 6 && !name.startsWith(".") && !name.equalsIgnoreCase("System Volume Information")) {
        pickRandomWav(full, depth + 1, skip, chosen, count);
      }
    } else if (!name.startsWith(".")) {             // skips macOS "._" junk files
      String lower = name;
      lower.toLowerCase();
      bool isChime = (dirPath == "/" && (lower == "lockchime.wav" || lower == "lockchime.tmp"));
      if (lower.endsWith(".wav") && !isChime && f.size() > 0 && f.size() < cfgMaxChime && full != skip && isGoodChimeWav(f)) {
        count++;
        if (esp_random() % count == 0) chosen = full;
      }
    }
    f = dir.openNextFile();
  }
}

void randomLockChime() {
  bool wasUsb = usbMode;
  showToast("Lock Chime", "Picking a file...", 10000);
  if (wasUsb) setUsbMode(false);                    // take the card back from the host for a moment

  String chosen;
  uint32_t count = 0;
  pickRandomWav("/", 0, lastChime, chosen, count);  // first try: anything except last time's pick
  if (count == 0) pickRandomWav("/", 0, "", chosen, count);   // only one candidate: allow a repeat

  bool ok = false;
  String line;
  if (count == 0) {
    line = "No suitable .wav files found";
  } else {
    String err = makeLockChime(chosen);
    if (err.length()) {
      line = err;
    } else {
      ok = true;
      lastChime = chosen;
      line = chosen.substring(chosen.lastIndexOf('/') + 1);
    }
  }

  if (wasUsb) setUsbMode(true);                     // hand the drive back to the host
  showToast(ok ? "Chime set" : "Chime failed", line, 3000, ok);
}

// ---------------- Settings from the web installer ----------------
// The installer page writes a small text block into a dedicated flash partition
// ("cfg", see partitions.csv) in the form:
//   WUSBCFG1
//   ssid=My Network
//   pass=mypassword
//   flip=1
//   ...
// If the partition is missing or doesn't start with the magic line (for example
// when flashed from the Arduino IDE or PlatformIO), the defaults above are used.
#define CFG_SIZE 4096

static void applyConfig(const String& k, const String& v) {
  if (k == "ssid")       { if (v.length() >= 1 && v.length() <= 32) cfgSsid = v; }
  else if (k == "pass")  { if (v.length() >= 8 && v.length() <= 63) cfgPass = v; }
  else if (k == "flip")  { cfgRotation = (v == "1") ? 1 : 3; }       // Flip screen? Yes = 1, No = 3
  else if (k == "label") { if (v.length() >= 1 && v.length() <= 11) cfgLabel = v; }
  else if (k == "usb")   { cfgStartUsb = (v == "1"); }
  else if (k == "chime") { long n = v.toInt(); if (n > 0) cfgMaxChime = (uint32_t)n; }
  else if (k == "lcd")   { cfgDisplay = (v == "1"); }                 // 0 = no screen fitted
  else if (k == "offx")  { cfgOffX = (int)v.toInt(); }
  else if (k == "offy")  { cfgOffY = (int)v.toInt(); }
  else if (k == "inv")   { cfgInvert = (v == "1"); }
  else if (k == "rgb")   { cfgRgb = (v == "1"); }
  else if (k == "bl")    { cfgBlOn = (v == "1") ? HIGH : LOW; }
  else if (k == "hid")   { cfgHidden = (v == "1"); }
  else if (k == "ch")    { long c = v.toInt(); if (c >= 1 && c <= 13) cfgChannel = (int)c; }
}

void loadConfig() {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x99, "cfg");
  if (!part) return;
  static uint8_t buf[CFG_SIZE];
  if (esp_partition_read(part, 0, buf, sizeof(buf)) != ESP_OK) return;
  if (memcmp(buf, "WUSBCFG1\n", 9) != 0) return;     // nothing (valid) written: keep defaults

  size_t i = 9;
  while (i < sizeof(buf) && buf[i] != 0 && buf[i] != 0xFF) {
    size_t j = i;
    while (j < sizeof(buf) && buf[j] != '\n' && buf[j] != 0 && buf[j] != 0xFF) j++;
    String line;
    line.concat((const char*)(buf + i), (unsigned int)(j - i));
    int eq = line.indexOf('=');
    if (eq > 0) applyConfig(line.substring(0, eq), line.substring(eq + 1));
    i = j + 1;
  }
}

// ---------------- Setup / loop ----------------
void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  sdLock = xSemaphoreCreateMutex();
  loadConfig();        // settings written by the web installer (if any)
  displayInit();

  if (!mountSD()) {
    // No card: keep the web UI up so you can see something is wrong.
    cardTotal = 0;
  } else {
    sectorCount = SD_MMC.numSectors();
    cardTotal = SD_MMC.totalBytes();
    readLayout(sectorCount);       // split card? then the car only sees the Boombox part
  }

  // USB mass storage
  msc.vendorID("ESP32");
  msc.productID(cfgLabel.c_str());
  msc.productRevision(FW_VERSION);
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(false);
  msc.begin(layout.hostSectors, 512);
  USB.productName(cfgLabel.c_str());
  USB.begin();

  // Wi-Fi access point + web server
  WiFi.persistent(false);          // we store credentials ourselves
  WiFi.mode(WIFI_AP_STA);          // own access point + optional router link
  WiFi.setAutoReconnect(true);
  WiFi.softAP(cfgSsid.c_str(), cfgPass.c_str(), cfgChannel, cfgHidden);
  loadWifiPrefs();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/card/split", HTTP_POST, handleCardSplit);
  server.on("/api/list", HTTP_GET, handleList);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUploadData);
  server.on("/rename", HTTP_POST, handleRename);
  server.on("/delete", HTTP_POST, handleDelete);
  server.on("/mkdir", HTTP_POST, handleMkdir);
  server.on("/lockchime", HTTP_POST, handleLockChime);
  server.on("/mode", HTTP_POST, handleMode);
  server.on("/wifi/scan", HTTP_POST, handleWifiScan);
  server.on("/api/wifi/networks", HTTP_GET, handleWifiNetworks);
  server.on("/api/wifi/status", HTTP_GET, handleWifiStatus);
  server.on("/wifi/connect", HTTP_POST, handleWifiConnect);
  server.on("/api/wifi/saved", HTTP_GET, handleWifiSaved);
  server.on("/wifi/delete", HTTP_POST, handleWifiDelete);
  server.on("/play", HTTP_GET, handlePlay);
  server.on("/api/wavinfo", HTTP_GET, handleWavInfo);
  server.begin();

  if (cfgStartUsb && sectorCount > 0) setUsbMode(true);
  else staConnectSaved();          // starting in Wi-Fi mode: join saved router
}

void loop() {
  server.handleClient();

  // Host ejected the drive -> switch to Wi-Fi edit mode
  if (ejectRequested) {
    ejectRequested = false;
    if (usbMode) setUsbMode(false);
  }

  if (splitChoice >= 0) doCardSplit();   // never returns (restarts the stick)

  freeScanTick();      // USB mode: count free clusters in small slices
  displayUpdate();     // redraws only when something changed

  // Finish a router connection attempt started from the web page
  if (staPending) {
    if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == pendingSsid) {
      addOrUpdateNet(pendingSsid, pendingPass, pendingHidden);   // only remembered once it works
      staPending = false;
      staResult = "";
    } else if (millis() - staStart > STA_TIMEOUT_MS) {
      staPending = false;
      staResult = "failed";
      WiFi.disconnect(false, false);
      if (!usbMode) staConnectSaved();           // go back to the remembered networks
    }
  }
  staAutoJoinTick();   // Wi-Fi mode: look for and join a remembered network

  // BOOT button: make a random .wav the lock chime
  static uint32_t lastPress = 0;
  static bool wasDown = false;
  bool down = (digitalRead(BUTTON_PIN) == LOW);
  if (down && !wasDown && millis() - lastPress > 500) {   // act once per press
    lastPress = millis();
    if (sectorCount > 0) randomLockChime();
  }
  wasDown = down;
}
