/*
 * Wi-Fi USB Stick  (ESP32-S3, Arduino-ESP32 core 3.x)
 * Version: see FW_VERSION below, and CHANGELOG.md
 *
 * Plugs into a car (or computer) and looks like a normal flash drive, backed
 * by a microSD card. Join the stick's Wi-Fi network, open http://192.168.4.1
 * and manage your sounds / choose the car's lock chime.
 *
 * Card layout: the SD card is divided into a 4 GB "Boombox" partition (the
 * car's drive) and a "CHIME_STORE" partition (the sound library, mounted at
 * /store). See the CARD LAYOUT section. A card without this layout is erased
 * and set up when the stick starts (see First Run).
 *
 * IMPORTANT: only ONE side may use a partition at a time, otherwise its FAT
 * file system gets corrupted. So the stick has two modes:
 *
 *   USB mode  - the car has the Boombox drive; the stick has the library.
 *   PC Mode   - the stick has the Boombox drive (hidden); a computer on the USB port
 *               is shown the library as a drive (the web page can take it back).
 *
 * Switch modes with the web page button. Ejecting a drive on the host makes
 * the stick take that drive back.
 *
 * Settings: the values in the config block below are only DEFAULTS. The web
 * installer can write a small "cfg" block into flash (Wi-Fi name/password,
 * screen flip, drive name, ...) which overrides them at boot (see loadConfig).
 *
 * BOOT button: a short press picks a random .wav (only from the categories marked
 * Random, see "Sound library database"; smaller than the size
 * limit) from the library, copies it to /LockChime.wav on the car's drive and
 * restarts the stick. Holding it for more than 5 seconds only restarts.
 *
 * Changing the chime always ends with a restart: the car only reads the new
 * sound when the stick looks unplugged and plugged in again. Each step is
 * shown on the screen.
 *
 * Wi-Fi: the stick always runs its own access point. It can also join your
 * router, in every mode: use the "Wi-Fi network" panel on the web page to
 * scan, enter a password and connect. Credentials are saved in flash.
 *
 * Display (T-Dongle S3 with LCD): mode, how full the sound library is, which
 * network the address is on, and the address to open.
 * Needs the "LovyanGFX" library by lovyan03 (declared in platformio.ini).
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
#include <ESPmDNS.h>
#include <vector>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Update.h>
#include "esp_mac.h"
#include <FS.h>
#include <SD_MMC.h>
#include "vfs_api.h"
#include "ff.h"
#include "diskio_impl.h"
#include "esp_vfs_fat.h"
#include <Preferences.h>
#include "esp_partition.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "USB.h"
#include "USBMSC.h"

// ---------------- Config ----------------
const char* AP_SSID = "Tesla-USB-Stick";
const char* AP_PASS = "s3xyandiknowit";   // at least 8 characters
#define FW_VERSION "0.20"               // bump for every release (also listed in CHANGELOG.md)
#define HOST_PREFIX "TeslaWiFiUSBStick" // network name of the stick; the last 3 bytes of its MAC are added: TeslaWiFiUSBStick-D4F1A2
// Where the stick looks for updates: the GitHub Pages site of this repository, which the build workflow
// fills with latest.json and the firmware (see "Firmware updates" below). Must end with a slash.
#define UPDATE_BASE_URL "https://peepsnet.github.io/tesla-wifi-usb-stick/"
#define VOLUME_LABEL "Boombox"      // name shown for the drive (max 11 characters)
#define BUTTON_PIN 0                   // BOOT button on most S3 boards
#define BUTTON_LONG_MS 5000            // hold this long to just restart (a short press picks a random lock chime)
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
volatile bool usbMode = false;        // true: the car has its drive. false: PC Mode, the stick has it
// What the USB host (car or computer) is shown right now:
//   HOST_CAR    the Boombox drive (always and only in USB mode)
//   HOST_STORE  the sound library, as a drive for a computer (PC Mode, when chosen)
//   HOST_NONE   nothing: the drive looks ejected
enum HostView { HOST_NONE, HOST_CAR, HOST_STORE };
volatile HostView hostView = HOST_NONE;
volatile bool ejectRequested = false;
uint32_t sectorCount = 0;
uint64_t cardTotal = 0;
String   cardProblem;                // shown instead of the drive usage when the card can't be used
bool     firstRun = false;           // the card is not set up yet: First Run will erase and format it
int      fmtPercent = 0;             // First Run progress
File uploadFile;

// Runtime settings. They start at the defaults from the config block at the top
// of this file and are overridden by the "cfg" flash block the web installer can
// write (see loadConfig() near the end of this file).
String   cfgSsid = AP_SSID;
String   cfgPass = AP_PASS;
String   cfgLabel = VOLUME_LABEL;
uint32_t cfgMaxChime = MAX_CHIME_BYTES;
int      cfgRotation = LCD_ROTATION;
int      cfgOffX = LCD_OFFSET_X, cfgOffY = LCD_OFFSET_Y;
bool     cfgInvert = LCD_INVERT, cfgRgb = LCD_RGB_ORDER;
uint8_t  cfgBlOn = LCD_BL_ON;
bool     cfgHidden = false;     // hide the stick's own Wi-Fi network name
int      cfgChannel = 1;        // channel of the stick's own Wi-Fi network (1-13)
bool     cfgDisplay = USE_DISPLAY;   // false for a dongle without a screen

String   hostName = HOST_PREFIX;     // set at start-up from the MAC address (initHostName)

// Firmware updates (see "Firmware updates" below)
enum UpdState { UPD_IDLE, UPD_CHECKING, UPD_DOWNLOADING, UPD_DONE, UPD_ERROR };
volatile UpdState updState = UPD_IDLE;
String   updLatest;                  // newest version published on GitHub ("" until a check has worked)
String   updMd5, updFile;            // from latest.json
uint32_t updSize = 0;
String   updErr;                     // why the last check/install failed
volatile int updPct = 0;             // install progress
bool     updAvail = false;           // a newer version than FW_VERSION is published (the screen shows an icon)
volatile bool updCheckRequested = false, updInstallRequested = false, updRunning = false;

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

// Sectors on the whole card. (SD_MMC.numSectors() is NOT this: it is the size of
// the mounted file system, i.e. only the first partition.)
uint32_t cardSectors() {
  uint64_t n = SD_MMC.cardSize() / 512;
  return n > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (uint32_t)n;
}

// A card that SD_MMC can't mount (brand new, exFAT, damaged file system) is
// opened here without a file system, so First Run can still format it.
static sdmmc_card_t* rawCard = nullptr;
bool rawCardOpen() {
  static sdmmc_card_t card;
  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.flags = SDMMC_HOST_FLAG_1BIT;
  host.max_freq_khz = SDMMC_FREQ_DEFAULT;
  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = (gpio_num_t)SD_CLK;
  slot.cmd = (gpio_num_t)SD_CMD;
  slot.d0 = (gpio_num_t)SD_D0;
  slot.d1 = slot.d2 = slot.d3 = GPIO_NUM_NC;
  slot.width = 1;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
  if (sdmmc_host_init() != ESP_OK) return false;
  if (sdmmc_host_init_slot(host.slot, &slot) != ESP_OK || sdmmc_card_init(&host, &card) != ESP_OK) {
    sdmmc_host_deinit();
    return false;
  }
  rawCard = &card;
  return true;
}
void rawCardClose() {
  if (!rawCard) return;
  rawCard = nullptr;
  sdmmc_host_deinit();
}
uint32_t rawCardSectors() {
  if (!rawCard) return 0;
  uint64_t n = (uint64_t)rawCard->csd.capacity * rawCard->csd.sector_size / 512;
  return n > 0xFFFFFFFFULL ? 0xFFFFFFFFUL : (uint32_t)n;
}

// Raw one-sector access used by the card layout code below.
bool formatting = false;                  // true while the card is being set up
static uint32_t fmtWrites = 0;
bool cardRead(uint32_t lba, uint8_t* buf) {
  if (rawCard) return sdmmc_read_sectors(rawCard, buf, lba, 1) == ESP_OK;
  return SD_MMC.readRAW(buf, lba);
}
bool cardWrite(uint32_t lba, const uint8_t* buf) {
  if (formatting && (++fmtWrites & 127) == 0) {   // during the long format: let other tasks
    delay(1);                                     // run and (First Run) keep the progress page answering
    if (firstRun) server.handleClient();
  }
  if (rawCard) return sdmmc_write_sectors(rawCard, buf, lba, 1) == ESP_OK;
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
// the storage partition. Every card is laid out like this: a card that isn't
// (new, used elsewhere, any other format) is erased and set up by First Run.
//
// This section only uses three hooks, defined before it:
//   bool cardRead(uint32_t lba, uint8_t* buf)          read one 512-byte sector
//   bool cardWrite(uint32_t lba, const uint8_t* buf)   write one 512-byte sector
//   void formatProgress(int percent)                   progress while formatting

#define BOOMBOX_START  2048UL          // 1 MB, the usual first-partition offset
#define PART_ALIGN     8192UL          // 4 MB alignment (good for SD card speed)
#define STORE_LABEL    "CHIME_STORE"   // 11 characters, the name of the sound library volume
#define MIN_STORE_SECT 2097152UL       // keep at least 1 GB for storage

struct CardLayout {
  bool     split = false;
  bool     readError = false;          // the card could not be read: layout unknown
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

// A failed read must never be mistaken for "not split" (that would erase the
// card), so reads that decide the layout are tried a few times.
static bool clReadRetry(uint32_t lba, uint8_t* buf) {
  for (int i = 0; i < 4; i++) {
    if (cardRead(lba, buf)) return true;
    delay(50);
  }
  return false;
}

// Work out whether the card is split, and what the car should be shown.
void readLayout(uint32_t totalSectors) {
  layout = CardLayout();
  layout.total = totalSectors;
  layout.hostSectors = totalSectors;
  uint8_t b[512];
  if (totalSectors == 0) return;
  if (!clReadRetry(0, b)) { layout.readError = true; return; }
  if (b[510] != 0x55 || b[511] != 0xAA) return;

  const uint8_t* e2 = b + 462;                       // second partition entry
  uint32_t s = clRd32(e2 + 8), n = clRd32(e2 + 12);
  if (!clIsFat(e2[4]) || s < BOOMBOX_START || n == 0 || s + n > totalSectors || s + n < s) return;
  if (totalSectors - (s + n) > PART_ALIGN) return;   // storage must reach the end of the card

  uint8_t v[512];                                    // is it really our storage partition?
  if (!clReadRetry(s, v)) { layout.readError = true; return; }
  if (v[510] != 0x55 || v[511] != 0xAA) return;
  // (The volume label is not checked: a computer may rename or reformat the library.)
  if (memcmp(v + 82, "FAT32   ", 8) != 0) return;

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

// ---- What a computer sees in PC Mode: the sound library only ----
// The whole card is shown, with a partition table that lists just the storage
// partition (at its real position, so nothing in its file system has to change).
// Everything outside that partition reads as empty and ignores writes, so the
// computer can't touch the Boombox drive or the real partition table.
bool storeHostRead(uint32_t lba, uint8_t* buf) {
  if (!layout.split || lba >= layout.total) return false;
  if (lba >= layout.stStart && lba - layout.stStart < layout.stSize) return cardRead(lba, buf);
  memset(buf, 0, 512);
  if (lba == 0) {
    memcpy(buf + 446, layout.storeEntry, 16);
    buf[510] = 0x55;
    buf[511] = 0xAA;
  }
  return true;
}

bool storeHostWrite(uint32_t lba, const uint8_t* buf) {
  if (!layout.split || lba >= layout.total) return false;
  if (lba >= layout.stStart && lba - layout.stStart < layout.stSize) return cardWrite(lba, buf);
  return true;                                       // outside the library: accepted, not stored
}

static void clEntry(uint8_t* e, uint32_t start, uint32_t size) {
  memset(e, 0, 16);
  e[1] = 0xFE; e[2] = 0xFF; e[3] = 0xFF;                                // CHS unused (LBA only)
  e[4] = 0x0C;                                                          // FAT32 (LBA)
  e[5] = 0xFE; e[6] = 0xFF; e[7] = 0xFF;
  clWr32(e + 8, start);
  clWr32(e + 12, size);
}

// The Boombox partition is 4 GB; a card too small for that is split in half.
uint32_t boomboxSectors(uint32_t total) {
  uint32_t usable = total > BOOMBOX_START ? total - BOOMBOX_START : 0;
  uint32_t want = 8388608UL;
  if (want + MIN_STORE_SECT + PART_ALIGN > usable) want = usable / 2;   // small card: split it in half
  return want - want % PART_ALIGN;
}

// Erase the whole card and split it. Returns "" on success, or what went wrong.
// volIdSeed should be something random (for example esp_random()).
const char* formatSplitCard(uint32_t total, uint32_t volIdSeed) {
  uint32_t bb = boomboxSectors(total);
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

// Erase and re-create only the Boombox partition (the car's drive). The storage
// partition and its files are not touched. Returns "" on success.
const char* formatBoombox(uint32_t volIdSeed) {
  if (!layout.split || layout.stStart <= BOOMBOX_START) return "The card is not set up";
  uint32_t size = layout.stStart - BOOMBOX_START;
  if (size > PART_ALIGN) size -= size % PART_ALIGN;
  Fat32Plan pb;
  if (!planFat32(BOOMBOX_START, size, pb)) return "Card too small for the Boombox partition";

  uint64_t all = planWrites(pb) + 16, done = 0;
  formatProgress(0);
  if (!writeFat32(pb, "Boombox", volIdSeed, done, all)) return "Write error (Boombox)";

  uint8_t m[512];                                    // partition table: Boombox + the kept storage entry
  if (!clReadRetry(0, m)) return "Read error (partition table)";
  memset(m + 446, 0, 64);
  clEntry(m + 446, BOOMBOX_START, size);
  memcpy(m + 462, layout.storeEntry, 16);
  m[510] = 0x55; m[511] = 0xAA;
  if (!cardWrite(0, m)) return "Write error (partition table)";
  layout.bbStart = BOOMBOX_START;
  layout.bbSize = size;
  formatProgress(100);
  return "";
}
// ==== CARD LAYOUT END ====

// The SD card's FatFs drive number. SD_MMC keeps it in a protected member, so
// read it through a derived class (no object of this class is ever made).
struct SDMMCPeek : public SDMMCFS {
  static uint8_t drive() {
    if (SD_MMC.*(&SDMMCPeek::_card) == nullptr) return 0xFF;
    return SD_MMC.*(&SDMMCPeek::_pdrv);
  }
};
uint8_t sdCardDrive() { return SDMMCPeek::drive(); }

// The stick keeps a copy of the current lock chime in the sound library, so it
// can put it back after the car's drive has been re-created.
#define CHIME_BACKUP "/.LockChime.wav"
bool restoreLockChime();
void libInvalidate();                   // the sound library database must be read again (see "Sound library database")

// ==== STORE FS BEGIN ====
// The CHIME_STORE partition of a split card, mounted at /store as a second FAT
// file system. FatFs reaches it through the small disk driver below, which adds
// the partition's start sector and hands the request to the SD card's own
// driver. The car never sees this part of the card, so it can be used in USB
// mode and in Wi-Fi mode alike.
//
// Uses, defined before this section: layout (CARD LAYOUT), sdLock, and
// uint8_t sdCardDrive() which returns the SD card's FatFs drive number (0xFF if none).

static uint8_t storeDrive = 0xFF;      // FatFs drive number of /store
bool storeMounted = false;

static DSTATUS storeInit(unsigned char) { return 0; }
static DSTATUS storeStatus(unsigned char) { return 0; }

static DRESULT storeRead(unsigned char, unsigned char* buf, uint32_t sector, unsigned count) {
  if (!layout.split || (uint64_t)sector + count > layout.stSize) return RES_PARERR;
  uint8_t sd = sdCardDrive();
  if (sd == 0xFF) return RES_NOTRDY;
  xSemaphoreTake(sdLock, portMAX_DELAY);
  DRESULT r = disk_read(sd, buf, layout.stStart + sector, count);
  xSemaphoreGive(sdLock);
  return r;
}

static DRESULT storeWrite(unsigned char, const unsigned char* buf, uint32_t sector, unsigned count) {
  if (!layout.split || (uint64_t)sector + count > layout.stSize) return RES_PARERR;
  uint8_t sd = sdCardDrive();
  if (sd == 0xFF) return RES_NOTRDY;
  xSemaphoreTake(sdLock, portMAX_DELAY);
  DRESULT r = disk_write(sd, buf, layout.stStart + sector, count);
  xSemaphoreGive(sdLock);
  return r;
}

static DRESULT storeIoctl(unsigned char, unsigned char cmd, void* buf) {
  switch (cmd) {
    case CTRL_SYNC:        return RES_OK;             // writes go straight to the card
    case GET_SECTOR_COUNT: *(LBA_t*)buf = layout.stSize; return RES_OK;
    case GET_SECTOR_SIZE:  *(WORD*)buf = 512; return RES_OK;
    case GET_BLOCK_SIZE:   *(DWORD*)buf = 1; return RES_OK;
    case CTRL_TRIM:        return RES_OK;
  }
  return RES_PARERR;
}

class StoreFS : public fs::FS {
public:
  StoreFS() : fs::FS(fs::FSImplPtr(new VFSImpl())) {}

  bool begin(const char* mountpoint = "/store") {
    if (storeMounted) return true;
    if (!layout.split) return false;
    BYTE pdrv = 0xFF;
    if (ff_diskio_get_drive(&pdrv) != ESP_OK || pdrv == 0xFF) return false;
    static const ff_diskio_impl_t impl = { &storeInit, &storeStatus, &storeRead, &storeWrite, &storeIoctl };
    ff_diskio_register(pdrv, &impl);
    char drv[3] = { (char)('0' + pdrv), ':', 0 };
    esp_vfs_fat_conf_t conf = { mountpoint, drv, 8 };
    FATFS* fs = nullptr;
    if (esp_vfs_fat_register_cfg(&conf, &fs) != ESP_OK) {
      ff_diskio_register(pdrv, NULL);
      return false;
    }
    if (f_mount(fs, drv, 1) != FR_OK) {               // 1 = check the file system now
      esp_vfs_fat_unregister_path(mountpoint);
      ff_diskio_register(pdrv, NULL);
      return false;
    }
    storeDrive = pdrv;
    storeMounted = true;
    _impl->mountpoint(mountpoint);
    return true;
  }

  void end() {
    if (!storeMounted) return;
    char drv[3] = { (char)('0' + storeDrive), ':', 0 };
    f_mount(NULL, drv, 0);
    esp_vfs_fat_unregister_path(_impl->mountpoint());
    ff_diskio_register(storeDrive, NULL);
    _impl->mountpoint(NULL);
    storeDrive = 0xFF;
    storeMounted = false;
  }

  // Size and free space in bytes (free space comes from the FSInfo sector, so it is quick).
  bool space(uint64_t& total, uint64_t& freeBytes) {
    if (!storeMounted) return false;
    char drv[3] = { (char)('0' + storeDrive), ':', 0 };
    FATFS* fs = nullptr;
    DWORD freeClusters = 0;
    if (f_getfree(drv, &freeClusters, &fs) != FR_OK || !fs) return false;
    total = (uint64_t)(fs->n_fatent - 2) * fs->csize * 512;
    freeBytes = (uint64_t)freeClusters * fs->csize * 512;
    return true;
  }
};
StoreFS STORE;
// ==== STORE FS END ====

// ---------------- Wi-Fi client helpers ----------------
// Up to MAX_NETS networks are remembered in flash. In every mode the stick scans
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

// Look for a remembered network soon.
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

// Runs from loop(), in every mode: when not connected to a router, scan every
// so often and join the strongest remembered network that is visible.
void staAutoJoinTick() {
  if (staPending || netCount == 0) return;
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
  if (hostView == HOST_NONE) return -1;
  xSemaphoreTake(sdLock, portMAX_DELAY);
  HostView v = hostView;                      // may have changed while waiting
  bool ok = v != HOST_NONE;
  uint8_t* p = (uint8_t*)buffer;
  for (uint32_t i = 0; i < bufsize / 512 && ok; i++) {
    // the car only sees its own part of the card; a computer in PC Mode only the library
    ok = v == HOST_CAR ? hostRead(lba + i, p + i * 512) : storeHostRead(lba + i, p + i * 512);
  }
  xSemaphoreGive(sdLock);
  return ok ? (int32_t)bufsize : -1;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (hostView == HOST_NONE) return -1;
  usbWriteCount = usbWriteCount + 1;   // only this callback writes it; loop() just reads
  xSemaphoreTake(sdLock, portMAX_DELAY);
  HostView v = hostView;
  bool ok = v != HOST_NONE;
  for (uint32_t i = 0; i < bufsize / 512 && ok; i++) {
    ok = v == HOST_CAR ? hostWrite(lba + i, buffer + i * 512) : storeHostWrite(lba + i, buffer + i * 512);
  }
  xSemaphoreGive(sdLock);
  return ok ? (int32_t)bufsize : -1;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject) {
  // Host pressed "Eject" -> the stick takes that drive back (done in loop()).
  if (load_eject && !start) ejectRequested = true;
  return true;
}

// ---------------- Free space while a computer has the library ----------------
// While the host owns the library we must not use FatFs on it, so we read its FAT
// table directly (read-only) and count unused clusters. This runs in small
// slices from loop(), only after the host has been idle for a few seconds.
static uint32_t le16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t* p) {
  return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool readSector(uint32_t lba, uint8_t* buf) {
  xSemaphoreTake(sdLock, portMAX_DELAY);
  bool ok = hostView == HOST_STORE && cardRead(lba, buf);
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
  uint32_t part = layout.stStart;

  if (!readSector(part, b)) { lastWriteMs = millis(); return; }   // retry in a few seconds
  if (!looksLikeBootSector(b)) { freeState = 2; return; }         // e.g. reformatted as exFAT

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
  if (hostView != HOST_STORE) return;
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
static const uint32_t C_GREEN = 0x00FF00, C_ORANGE = 0xFFA000, C_GREY = 0xB0B0B0, C_RED = 0xFF3030;
String lastScreenKey;

// Short pop-up message on the screen
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

  // 160 x 80 layout:
  //   [ Tesla WiFi USB Stick   v0.8 ]   header bar: orange while the car (USB host)
  //     42% used           of 55GB       is using the drive, cyan in PC Mode
  //   [#########.................. ]   how full the sound library is
  //     CAR HAS DRIVE     STICK WIFI     mode, and which network the address is on
  //            192.168.4.1
  uint32_t titleColor = usbMode ? C_ORANGE : C_CYAN;

  // How full the sound library is. Normally the stick asks its file system
  // (cached); while a computer has the library it counts free clusters itself.
  String usage, sizeText;
  int pct = -1;                                      // -1: no figure, show the text in `usage`
  static uint64_t libTotal = 0, libUsed = 0;
  static uint32_t libAt = 0;
  uint64_t total = 0, used = 0;
  bool known = false;
  if (cardProblem.length()) {
    usage = cardProblem;
  } else if (sectorCount == 0) {
    usage = "No SD card";
  } else if (hostView == HOST_STORE) {
    libAt = 0;
    if (freeState == 1 && fsTotalBytes > 0) {
      total = fsTotalBytes;
      used = fsTotalBytes - freeBytes;
      known = true;
    } else {
      usage = freeState == 2 ? "Library: not FAT32" : "Checking space...";
    }
  } else if (storeMounted) {
    if (libAt == 0 || millis() - libAt > 5000) {
      uint64_t t = 0, f = 0;
      if (STORE.space(t, f)) { libTotal = t; libUsed = t - f; }
      libAt = millis() | 1;
    }
    total = libTotal;
    used = libUsed;
    known = total > 0;
    if (!known) usage = "Checking space...";
  } else {
    usage = "Library unavailable";
  }
  if (known) {
    pct = (int)((used * 100 + total / 2) / total);
    if (pct > 100) pct = 100;
    double gib = total / 1073741824.0;
    sizeText = "of " + (gib >= 1.0 ? String((int)(gib + 0.5)) + "GB" : String((int)(total / 1048576)) + "MB");
    usage = String(pct) + "% used";
  }

  // The address to open: the one from your router when joined, otherwise the stick's own network.
  bool joined = WiFi.status() == WL_CONNECTED;
  String ip = (joined ? WiFi.localIP() : WiFi.softAPIP()).toString();

  String key = String(titleColor) + "|" + usage + sizeText + "|" + ip + (joined ? "J" : "A") + (updAvail ? "U" : "");
  if (key == lastScreenKey) return;      // nothing changed: no redraw, no flicker
  lastScreenKey = key;

  const int W = lcdPtr->width();
  auto text = [&](const String& t, int x, int y) { lcdPtr->setCursor(x, y); lcdPtr->print(t.c_str()); };
  auto right = [&](const String& t, int y) { text(t, W - 4 - lcdPtr->textWidth(t.c_str()), y); };
  auto centre = [&](const String& t, int y) { text(t, (W - lcdPtr->textWidth(t.c_str())) / 2, y); };

  lcdPtr->fillScreen(C_BLACK);
  lcdPtr->setTextSize(1);

  // Header bar (6 x 8 font, dark text on the mode colour)
  lcdPtr->fillRect(0, 0, W, 16, titleColor);
  lcdPtr->setFont(&fonts::Font0);
  lcdPtr->setTextColor(C_BLACK, titleColor);
  if (updAvail) {
    // A newer firmware is published: shorter title, and a white "arrow up" badge where the rest of it was.
    text("Tesla WiFi USB", 4, 4);
    const int cx = 102, cy = 8;
    lcdPtr->fillCircle(cx, cy, 7, C_WHITE);
    lcdPtr->fillTriangle(cx, cy - 5, cx - 5, cy, cx + 5, cy, C_BLACK);
    lcdPtr->fillRect(cx - 2, cy, 5, 5, C_BLACK);
  } else {
    text("Tesla WiFi USB Stick", 4, 4);
  }
  right("v" FW_VERSION, 4);

  // Sound library: figure, size and a bar; or one line saying what is wrong
  lcdPtr->setFont(&fonts::Font2);                  // 16 px tall
  lcdPtr->setTextColor(C_WHITE, C_BLACK);
  if (pct >= 0) {
    text(usage, 4, 18);
    lcdPtr->setTextColor(C_GREY, C_BLACK);
    right(sizeText, 18);
    uint32_t barColor = pct >= 95 ? C_RED : pct >= 80 ? C_ORANGE : C_GREEN;
    lcdPtr->drawRect(4, 36, W - 8, 8, C_GREY);
    int fill = (W - 12) * pct / 100;
    if (pct > 0 && fill < 2) fill = 2;             // anything on the drive shows
    if (fill > 0) lcdPtr->fillRect(6, 38, fill, 4, barColor);
  } else {
    if (lcdPtr->textWidth(usage.c_str()) > W - 8) lcdPtr->setFont(&fonts::Font0);
    lcdPtr->setTextColor(cardProblem.length() || sectorCount == 0 ? C_ORANGE : C_WHITE, C_BLACK);
    centre(usage, 24);
  }

  // Mode and network (small), then the address to open (large)
  lcdPtr->setFont(&fonts::Font0);
  lcdPtr->setTextColor(titleColor, C_BLACK);
  text(usbMode ? "CAR MODE" : "PC MODE", 4, 50);
  lcdPtr->setTextColor(C_GREY, C_BLACK);
  right(joined ? "HOME WIFI" : "STICK WIFI", 50);
  lcdPtr->setFont(&fonts::Font2);
  if (lcdPtr->textWidth(ip.c_str()) > W - 8) lcdPtr->setFont(&fonts::Font0);
  lcdPtr->setTextColor(C_WHITE, C_BLACK);
  centre(ip, 62);
  lcdPtr->setFont(&fonts::Font0);                  // the pop-up messages use the basic font
}

// A full-screen notice in large type (First Run, card setup): a header bar, a
// line, one big figure and a footer. Stays up for `ms`, like a pop-up message.
void showBig(const char* header, const String& line, const String& big, const String& foot, uint32_t color, uint32_t ms) {
  toastUntil = millis() + ms;
  if (!lcdPtr) return;
  const int W = lcdPtr->width();
  auto centre = [&](const String& t, int y) {
    lcdPtr->setCursor((W - lcdPtr->textWidth(t.c_str())) / 2, y);
    lcdPtr->print(t.c_str());
  };
  lcdPtr->fillScreen(C_BLACK);
  lcdPtr->setTextSize(1);
  lcdPtr->fillRect(0, 0, W, 18, color);
  lcdPtr->setFont(&fonts::Font2);                  // 16 px tall
  lcdPtr->setTextColor(C_BLACK, color);
  centre(header, 1);
  lcdPtr->setTextColor(C_WHITE, C_BLACK);
  centre(line, 20);
  lcdPtr->setFont(&fonts::Font4);                  // 26 px tall
  lcdPtr->setTextColor(color, C_BLACK);
  centre(big, 37);
  lcdPtr->setFont(&fonts::Font2);
  lcdPtr->setTextColor(C_GREY, C_BLACK);
  centre(foot, 63);
  lcdPtr->setFont(&fonts::Font0);                  // the pop-up messages use the basic font
}

void formatProgress(int percent) {
  static int last = -1;
  if (percent == last) return;
  last = percent;
  fmtPercent = percent;
  showBig(firstRun ? "First Run" : "Card setup", "Formatting card", String(percent) + "%", "Do not unplug", C_ORANGE, 600000);
}

// ---------------- Mode switching ----------------
// The sound library can be lent to a computer on the USB port as a drive (edit
// mode only). While it is, the stick keeps its hands off it: STORE is unmounted.
void takeStoreBack() {
  if (hostView != HOST_STORE) return;
  msc.mediaPresent(false);        // the computer sees the drive removed
  delay(600);                     // give it time to notice
  xSemaphoreTake(sdLock, portMAX_DELAY);
  hostView = HOST_NONE;
  xSemaphoreGive(sdLock);
  STORE.begin();                  // mounted fresh, so the computer's changes are seen
  libInvalidate();                // ... and so is the database of sounds and categories
}

void giveStoreToHost() {
  if (usbMode || !storeMounted) return;
  if (uploadFile) uploadFile.close();
  STORE.end();
  libInvalidate();
  freeReset();                    // count the library's free space again
  msc.begin(layout.total, 512);   // the drive's size as the computer sees it
  xSemaphoreTake(sdLock, portMAX_DELAY);
  hostView = HOST_STORE;
  xSemaphoreGive(sdLock);
  msc.mediaPresent(true);
}

// USB mode: the car has the Boombox drive and the stick has the library.
// PC Mode: the stick has the Boombox drive (the host sees nothing, until the
// library is lent to it with giveStoreToHost()).
void setUsbMode(bool on) {
  if (on) takeStoreBack();
  if (on == usbMode) return;
  if (on) {
    if (uploadFile) uploadFile.close();
    xSemaphoreTake(sdLock, portMAX_DELAY);
    SD_MMC.end();                 // flush and drop cached FAT state
    mountSD();
    if (sectorCount > 0) setVolumeLabelRaw(cfgLabel.c_str());   // name the drive
    msc.begin(layout.hostSectors, 512);
    usbMode = true;
    hostView = HOST_CAR;
    xSemaphoreGive(sdLock);
    msc.mediaPresent(true);       // host sees the drive (re)appear
  } else {
    msc.mediaPresent(false);      // host sees the drive removed
    delay(600);                   // give the host time to notice
    xSemaphoreTake(sdLock, portMAX_DELAY);
    usbMode = false;
    hostView = HOST_NONE;
    SD_MMC.end();                 // remount fresh so we see the host's changes
    mountSD();
    xSemaphoreGive(sdLock);
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
    server.send(423, "text/plain", "The car has this drive. Switch to PC Mode first.");
    return false;
  }
  return true;
}

// Every file request names a volume: "store" (the sound library on a split card,
// usable in any mode) or anything else for the car's drive (Wi-Fi mode only).
bool isStoreVol() { return server.arg("vol") == "store"; }
fs::FS& volFS() { return isStoreVol() ? (fs::FS&)STORE : (fs::FS&)SD_MMC; }
bool requireVol() {
  if (isStoreVol()) {
    if (storeMounted) return true;
    server.send(409, "text/plain", hostView == HOST_STORE
      ? "The computer on the USB port is using the sound library. Take it back first."
      : "The sound library is not available (no usable SD card).");
    return false;
  }
  return requireWifiMode();
}

// ---------------- Sound library database (categories) ----------------
// Sounds can be sorted into categories ("Christmas Sounds", "Halloween Sounds", ...). A sound can be in any number
// of categories; one that is in none is in the built-in "UnCategorized". Every sound has a display Name and a file
// name; both can be edited. The stick's button picks its random chime only from the categories marked "Random".
//
// It is kept in one small text file on CHIME_STORE (hidden from the file list), one record per line, written to a
// temporary file first so that a power cut can't lose it:
//   WUSBLIB1
//   U|1                      UnCategorized: 1 = include in random picks
//   C|3|1|Christmas Sounds   category number (1..30), random flag, name
//   S|10|/Sound5.wav|Jingle  categories as a bit mask (bit n = category n), path in the library, display name
// Files that appear in the library by other means (a computer in PC Mode, the Library files tab) are added as
// UnCategorized; entries whose file has gone are dropped. Nothing here touches the car's Boombox drive.
#define LIB_DB      "/.sounds.db"
#define LIB_DB_TMP  "/.sounds.tmp"
#define MAX_CATS    30                 // category numbers 1..30; number 0 is "UnCategorized"
#define MAX_SOUNDS  1000
struct LibCat { bool used = false; bool rand = true; String name; };
struct LibSound { String path; String name; uint32_t mask = 0; };
static LibCat libCats[MAX_CATS + 1];
static std::vector<LibSound> libSounds;
static bool libLoaded = false;
static bool libStale = true;           // files may have been added or removed behind our back: scan again

static void libReset() {
  libSounds.clear();
  libSounds.shrink_to_fit();
  for (int i = 0; i <= MAX_CATS; i++) { libCats[i].used = false; libCats[i].rand = true; libCats[i].name = ""; }
  libCats[0].used = true;
  libCats[0].name = "UnCategorized";
}

void libInvalidate() {
  libLoaded = false;
  libStale = true;
  libReset();
}

// Display names and category names: no separator or control characters, trimmed, cut to maxLen bytes.
static String libCleanName(const String& in, size_t maxLen) {
  String o;
  for (size_t i = 0; i < in.length(); i++) {
    uint8_t c = (uint8_t)in[i];
    if (c == '|' || c < 32 || c == 127) continue;
    o += (char)c;
  }
  o.trim();
  if (o.length() > maxLen) {
    o = o.substring(0, maxLen);
    while (o.length() && ((uint8_t)o[o.length() - 1] & 0xC0) == 0x80) o.remove(o.length() - 1);   // don't leave half a character
    if (o.length() && (uint8_t)o[o.length() - 1] >= 0xC0) o.remove(o.length() - 1);
    o.trim();
  }
  return o;
}

static bool libNameChar(char c) {
  return isalnum((unsigned char)c) || c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')' ||
         c == '&' || c == '\'' || c == '#';
}

// "" if f is an acceptable file name for a sound, otherwise what is wrong with it.
static String libFileNameProblem(const String& f) {
  if (f.length() > 64) return "The file name is too long (64 characters at most).";
  String l = f;
  l.toLowerCase();
  if (f.length() < 5 || !l.endsWith(".wav")) return "The file name must end in .wav";
  String base = f.substring(0, f.length() - 4);
  if (base.startsWith(" ") || base.endsWith(" ")) return "The file name can't start or end with a space.";
  if (base.startsWith(".") || base.endsWith(".")) return "The file name can't start or end with a dot.";
  if (base.indexOf("..") >= 0) return "The file name can't contain two dots in a row.";
  for (size_t i = 0; i < base.length(); i++) {
    if (!libNameChar(base[i])) return "The file name can only use letters, numbers, spaces and _ - . ( ) & ' #";
  }
  return "";
}

// A name for an uploaded file: anything the list above doesn't allow becomes an underscore.
static String libSafeUploadName(const String& in) {
  String o;
  for (size_t i = 0; i < in.length(); i++) o += libNameChar(in[i]) ? in[i] : '_';
  return o;
}

static int libFind(const String& path) {
  for (size_t i = 0; i < libSounds.size(); i++) if (libSounds[i].path == path) return (int)i;
  return -1;
}

static uint32_t libUsedMask() {
  uint32_t m = 0;
  for (int i = 1; i <= MAX_CATS; i++) if (libCats[i].used) m |= (1UL << i);
  return m;
}

static bool libSave() {
  if (!storeMounted) return false;
  File f = STORE.open(LIB_DB_TMP, FILE_WRITE);
  if (!f) return false;
  f.print("WUSBLIB1\n");
  f.printf("U|%d\n", libCats[0].rand ? 1 : 0);
  for (int i = 1; i <= MAX_CATS; i++) {
    if (libCats[i].used) f.printf("C|%d|%d|%s\n", i, libCats[i].rand ? 1 : 0, libCats[i].name.c_str());
  }
  for (const LibSound& s : libSounds) f.printf("S|%lu|%s|%s\n", (unsigned long)s.mask, s.path.c_str(), s.name.c_str());
  f.close();
  STORE.remove(LIB_DB);
  return STORE.rename(LIB_DB_TMP, LIB_DB);
}

static int libSplit(const String& s, String* out, int max) {
  int n = 0, p = 0;
  while (n < max - 1) {
    int q = s.indexOf('|', p);
    if (q < 0) break;
    out[n++] = s.substring(p, q);
    p = q + 1;
  }
  out[n++] = s.substring(p);
  return n;
}

static void libLoad() {
  if (libLoaded) return;
  libLoaded = true;
  libStale = true;
  libReset();
  if (!storeMounted) return;
  File f = STORE.open(LIB_DB, FILE_READ);
  if (!f) f = STORE.open(LIB_DB_TMP, FILE_READ);       // a save that was cut short: the new copy is complete if it has its header
  if (!f) return;
  String line = f.readStringUntil('\n');
  line.trim();
  if (line != "WUSBLIB1") { f.close(); return; }
  while (f.available()) {
    line = f.readStringUntil('\n');
    line.trim();
    String p[4];
    if (line.startsWith("U|")) {
      libCats[0].rand = line.substring(2) != "0";
    } else if (line.startsWith("C|") && libSplit(line.substring(2), p, 3) == 3) {
      int id = p[0].toInt();
      if (id >= 1 && id <= MAX_CATS && p[2].length()) { libCats[id].used = true; libCats[id].rand = p[1] != "0"; libCats[id].name = p[2]; }
    } else if (line.startsWith("S|") && libSplit(line.substring(2), p, 3) == 3 && libSounds.size() < MAX_SOUNDS) {
      LibSound s;
      s.mask = (uint32_t)strtoul(p[0].c_str(), nullptr, 10);
      s.path = p[1];
      s.name = p[2];
      if (s.path.length() > 1 && s.path[0] == '/') libSounds.push_back(s);
    }
  }
  f.close();
  uint32_t used = libUsedMask();
  for (LibSound& s : libSounds) s.mask &= used;        // never point at a category that doesn't exist
}

// Collects the path of every .wav file in the library (folders are searched too).
static void libScan(const String& dirPath, int depth, std::vector<String>& found) {
  File dir = STORE.open(dirPath);
  if (!dir || !dir.isDirectory()) return;
  File f = dir.openNextFile();
  while (f) {
    String name = String(f.name());
    String full = (dirPath == "/" ? "" : dirPath) + "/" + name;
    if (f.isDirectory()) {
      if (depth < 6 && !name.startsWith(".") && !name.equalsIgnoreCase("System Volume Information")) libScan(full, depth + 1, found);
    } else if (!name.startsWith(".")) {
      String lower = name;
      lower.toLowerCase();
      if (lower.endsWith(".wav") && found.size() < MAX_SOUNDS) found.push_back(full);
    }
    f = dir.openNextFile();
  }
}

// Makes the database match the files that are really in the library.
static void libReconcile() {
  libLoad();
  if (!storeMounted || !libStale) return;
  libStale = false;
  std::vector<String> found;
  libScan("/", 0, found);
  bool changed = false;
  for (size_t i = libSounds.size(); i-- > 0;) {
    bool there = false;
    for (const String& p : found) if (p == libSounds[i].path) { there = true; break; }
    if (!there) { libSounds.erase(libSounds.begin() + i); changed = true; }
  }
  for (const String& p : found) {
    if (libFind(p) >= 0 || libSounds.size() >= MAX_SOUNDS) continue;
    LibSound s;
    s.path = p;
    String base = p.substring(p.lastIndexOf('/') + 1);
    s.name = libCleanName(base.substring(0, base.length() - 4), 60);
    if (s.name.length() == 0) s.name = base;
    libSounds.push_back(s);
    changed = true;
  }
  if (changed) libSave();
}

// A file or folder in the library was renamed or removed with the "Library files" tab.
static void libPathMoved(const String& from, const String& to) {
  libLoad();
  bool changed = false;
  for (LibSound& s : libSounds) {
    if (s.path == from) { s.path = to; changed = true; }
    else if (s.path.startsWith(from + "/")) { s.path = to + s.path.substring(from.length()); changed = true; }
  }
  if (changed) libSave();
}
static void libPathRemoved(const String& path) {
  libLoad();
  bool changed = false;
  for (size_t i = libSounds.size(); i-- > 0;) {
    if (libSounds[i].path == path || libSounds[i].path.startsWith(path + "/")) { libSounds.erase(libSounds.begin() + i); changed = true; }
  }
  if (changed) libSave();
}

// Rename a file, even if only the letter case changes (FAT needs a detour for that).
static bool libMoveFile(const String& from, const String& to) {
  String a = from, b = to;
  a.toLowerCase();
  b.toLowerCase();
  if (a != b) return STORE.rename(from, to);
  String tmp = from + ".tmp~";
  if (!STORE.rename(from, tmp)) return false;
  if (!STORE.rename(tmp, to)) { STORE.rename(tmp, from); return false; }
  return true;
}

static bool requireStore() {
  if (storeMounted) return true;
  server.send(409, "text/plain", hostView == HOST_STORE
    ? "The computer on the USB port is using the sound library. Take it back first."
    : "The sound library is not available (no usable SD card).");
  return false;
}

// GET /api/lib: {"cats":[{"i":0,"n":"UnCategorized","r":1},...],"sounds":[{"f":"/a.wav","n":"Name","c":mask},...]}
void handleLibList() {
  if (!requireStore()) return;
  libReconcile();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String out = "{\"cats\":[";
  bool first = true;
  for (int i = 0; i <= MAX_CATS; i++) {
    if (!libCats[i].used) continue;
    if (!first) out += ',';
    first = false;
    out += "{\"i\":" + String(i) + ",\"n\":\"" + jsonEscape(libCats[i].name) + "\",\"r\":" + String(libCats[i].rand ? 1 : 0) + "}";
  }
  out += "],\"sounds\":[";
  first = true;
  for (const LibSound& s : libSounds) {
    if (!first) out += ',';
    first = false;
    out += "{\"f\":\"" + jsonEscape(s.path) + "\",\"n\":\"" + jsonEscape(s.name) + "\",\"c\":" + String((unsigned long)s.mask) + "}";
    if (out.length() > 1000) { server.sendContent(out); out = ""; }
  }
  out += "]}";
  server.sendContent(out);
  server.sendContent("");
}

void handleLibCatAdd() {
  if (!requireStore()) return;
  libLoad();
  String name = libCleanName(server.arg("name"), 40);
  if (name.length() == 0) { server.send(400, "text/plain", "Give the category a name."); return; }
  for (int i = 0; i <= MAX_CATS; i++) {
    if (libCats[i].used && libCats[i].name.equalsIgnoreCase(name)) { server.send(409, "text/plain", "There is already a category with that name."); return; }
  }
  int id = 0;
  for (int i = 1; i <= MAX_CATS && !id; i++) if (!libCats[i].used) id = i;
  if (!id) { server.send(409, "text/plain", "That is the most categories there can be (" + String(MAX_CATS) + "). Delete one first."); return; }
  libCats[id].used = true;
  libCats[id].rand = true;
  libCats[id].name = name;
  if (!libSave()) { server.send(500, "text/plain", "Could not save the categories."); return; }
  server.send(200, "text/plain", String(id));
}

static int libCatArg() {
  int id = server.arg("id").toInt();
  return (id >= 0 && id <= MAX_CATS && libCats[id].used) ? id : -1;
}

void handleLibCatRename() {
  if (!requireStore()) return;
  libLoad();
  int id = libCatArg();
  if (id < 1) { server.send(400, "text/plain", "No such category."); return; }
  String name = libCleanName(server.arg("name"), 40);
  if (name.length() == 0) { server.send(400, "text/plain", "Give the category a name."); return; }
  for (int i = 0; i <= MAX_CATS; i++) {
    if (i != id && libCats[i].used && libCats[i].name.equalsIgnoreCase(name)) { server.send(409, "text/plain", "There is already a category with that name."); return; }
  }
  libCats[id].name = name;
  bool ok = libSave();
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Could not save the categories.");
}

void handleLibCatDelete() {
  if (!requireStore()) return;
  libLoad();
  int id = libCatArg();
  if (id < 1) { server.send(400, "text/plain", "No such category."); return; }
  for (LibSound& s : libSounds) s.mask &= ~(1UL << id);     // the sounds stay; those left in no category are UnCategorized
  libCats[id].used = false;
  libCats[id].name = "";
  bool ok = libSave();
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Could not save the categories.");
}

void handleLibCatRandom() {
  if (!requireStore()) return;
  libLoad();
  int id = libCatArg();
  if (id < 0) { server.send(400, "text/plain", "No such category."); return; }
  libCats[id].rand = server.arg("on") != "0";
  bool ok = libSave();
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Could not save the categories.");
}

// POST /lib/sound/set  path, name (display name), file (file name, no folder), mask (category bit mask)
void handleLibSoundSet() {
  if (!requireStore()) return;
  libReconcile();
  String path = safePath(server.arg("path"));
  int idx = libFind(path);
  if (idx < 0) { server.send(404, "text/plain", "That sound is not in the library any more."); return; }
  String name = libCleanName(server.arg("name"), 60);
  if (name.length() == 0) { server.send(400, "text/plain", "Give the sound a name."); return; }
  String file = server.arg("file");
  file.trim();
  int slash = path.lastIndexOf('/');
  String cur = path.substring(slash + 1);
  String newPath = path;
  if (file != cur) {                                        // an unchanged file name is left alone, whatever it looks like
    String why = libFileNameProblem(file);
    if (why.length()) { server.send(400, "text/plain", why); return; }
    newPath = path.substring(0, slash) + "/" + file;
    if (!file.equalsIgnoreCase(cur) && STORE.exists(newPath)) { server.send(409, "text/plain", "A file with that name already exists."); return; }
    if (!libMoveFile(path, newPath)) { server.send(500, "text/plain", "Could not rename the file."); return; }
  }
  libSounds[idx].path = newPath;
  libSounds[idx].name = name;
  libSounds[idx].mask = (uint32_t)strtoul(server.arg("mask").c_str(), nullptr, 10) & libUsedMask();
  bool ok = libSave();
  server.send(ok ? 200 : 500, "text/plain", ok ? "OK" : "Could not save the categories.");
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
 td.a{text-align:right;white-space:nowrap}
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
 #tesla .sm,#card .sm,#upd .sm{font-size:13px;color:#666}
 #updi{display:inline-block;vertical-align:middle;color:#0a7d2c;line-height:0;margin-left:6px}
 #updi svg{width:22px;height:22px;fill:currentColor}
 #updi[hidden]{display:none}
 #host{font-size:12px;color:#888;margin:-8px 0 8px}
 #updbar{width:100%;height:14px;margin:8px 0}
 #tabs{display:flex;gap:6px;margin:8px 0 2px}
 #tabs button{flex:1;padding:8px;border:1px solid #bbb;background:#f4f4f4;border-radius:6px;font-weight:600}
 #tabs button.on{background:#06c;color:#fff;border-color:#06c}
 #volinfo{font-size:13px;color:#555;margin:4px 0}
 button.ib{padding:4px 5px;margin:1px;line-height:0;vertical-align:middle}
 svg.ic{width:18px;height:18px;fill:currentColor}
 #legend{font-size:12px;color:#666;margin-top:10px}
 #legend span{white-space:nowrap;margin-right:10px;display:inline-block}
 #legend svg.ic{width:14px;height:14px;vertical-align:-2px}
 #up{display:none}
 .chk{display:block;font-size:14px;margin:2px 0}
 /* Bootstrap-like look, written out here because the stick has no internet to load Bootstrap from.
    Only the page's own buttons are styled; the small icon buttons in the file rows are left alone. */
 body{font-family:system-ui,-apple-system,"Segoe UI",Roboto,"Helvetica Neue",Arial,sans-serif;color:#212529;line-height:1.5}
 h2{font-weight:500;line-height:1.2}
 #scan,#wconn,#updcheck,#updgo,#updfilego,#rebuild,#tabs button,#tools button.ib{
  display:inline-block;font-weight:400;line-height:1.5;text-align:center;vertical-align:middle;cursor:pointer;
  border:1px solid transparent;border-radius:.375rem;padding:.375rem .75rem;margin:.15rem .25rem .15rem 0;font-size:1rem;
  transition:color .15s,background-color .15s,border-color .15s}
 #reboot{display:inline-block;font-weight:400;line-height:1.5;text-align:center;vertical-align:middle;cursor:pointer;
  border:1px solid #6c757d;border-radius:.375rem;padding:.375rem .75rem;margin:.15rem .25rem .15rem 0;font-size:1rem;color:#6c757d;background:transparent}
 #reboot:hover{color:#fff;background:#6c757d}
 #wconn,#tabs button.on{color:#fff;background:#0d6efd;border-color:#0d6efd}
 #wconn:hover,#tabs button.on:hover{background:#0b5ed7;border-color:#0a58ca}
 #updgo,#updfilego{color:#fff;background:#198754;border-color:#198754}
 #updgo:hover,#updfilego:hover{background:#157347;border-color:#146c43}
 #scan,#updcheck,#tabs button{color:#6c757d;background:transparent;border-color:#6c757d}
 #scan:hover,#updcheck:hover,#tabs button:hover{color:#fff;background:#6c757d}
 #tabs button.on{color:#fff;background:#0d6efd;border-color:#0d6efd}
 #rebuild{color:#fff;background:#dc3545;border-color:#dc3545}
 #rebuild:hover{background:#bb2d3b;border-color:#b02a37}
 #tools button.ib{color:#0d6efd;background:transparent;border-color:#0d6efd;padding:.25rem .5rem}
 #tools button.ib:hover{color:#fff;background:#0d6efd}
 #scan:disabled,#wconn:disabled,#updcheck:disabled,#updgo:disabled,#updfilego:disabled,#rebuild:disabled{opacity:.65;cursor:default}
 #tabs{gap:0;margin:14px 0 0}
 #tabs button{flex:1;margin:0;border-radius:0;font-size:1.05rem}
 #tabs button:first-child{border-radius:.375rem 0 0 0}
 #tabs button+button{margin-left:-1px}
 #tabs button:last-child{border-radius:0 .375rem 0 0}
 #panel{border:1px solid #dee2e6;border-radius:0 0 .375rem .375rem;padding:.5rem .75rem .75rem;margin-bottom:1rem}
 #panel #card{margin-top:1rem}
 #subtabs{display:flex;gap:.4rem;margin:.2rem 0 .4rem}
 #subtabs button{margin:0;padding:.25rem .9rem;font-size:.95rem;border:1px solid #0d6efd;border-radius:2rem;color:#0d6efd;background:#fff;cursor:pointer}
 #subtabs button:hover{background:#e7f1ff}
 #subtabs button.on{color:#fff;background:#0d6efd}
 #tabs button{font-weight:400}
 #bar{padding:1rem;border:1px solid #9ec5fe;border-radius:.375rem;background:#cfe2ff;color:#052c65;margin-bottom:1rem}
 #bar.usb{background:#fff3cd;border-color:#ffe69c;color:#664d03}
 #modehelp{font-size:.875rem;margin-top:.4rem;opacity:.85}
 details{background:#fff;border:1px solid #dee2e6;border-radius:.375rem;padding:.6rem .9rem}
 details[open]>summary{margin-bottom:.4rem}
 details input[type=text],details input[type=password],details select{border:1px solid #ced4da;border-radius:.375rem;padding:.375rem .75rem;font-size:1rem}
 details input[type=file]{margin:.25rem 0}
 progress{accent-color:#0d6efd}
 .bb{display:inline-block;font-weight:400;line-height:1.5;text-align:center;cursor:pointer;border:1px solid #6c757d;border-radius:.375rem;
  padding:.375rem .75rem;margin:.15rem .25rem .15rem 0;font-size:1rem;color:#6c757d;background:transparent}
 .bb:hover{color:#fff;background:#6c757d}
 .bb-primary{color:#fff;background:#0d6efd;border-color:#0d6efd}
 .bb-primary:hover{background:#0b5ed7;border-color:#0a58ca}
 .bb-success{color:#fff;background:#198754;border-color:#198754}
 .bb-success:hover{background:#157347;border-color:#146c43}
 /* a category is a card: a solid coloured header bar, with its sounds on white underneath */
 #cats details.cat{padding:0;overflow:hidden;border:1px solid #0d6efd;border-radius:.5rem;margin-bottom:1.1rem;background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.15)}
 #cats details.cat.un{border-color:#6c757d}
 #cats details.cat[open]>summary{margin-bottom:0}
 #cats summary{display:flex;align-items:center;gap:.5rem;flex-wrap:wrap;list-style:none;background:#0d6efd;color:#fff;padding:.6rem .75rem;font-size:1.1rem;font-weight:600}
 #cats details.un>summary{background:#6c757d}
 #cats summary::-webkit-details-marker{display:none}
 #cats summary::before{content:"\25BE";font-size:.9rem}
 #cats details.cat:not([open])>summary::before{content:"\25B8"}
 #cats .cn{flex:1;min-width:8rem}
 #cats .cc{display:inline-block;margin-left:.35rem;padding:.05rem .55rem;border-radius:1rem;background:rgba(255,255,255,.28);font-size:.8rem;font-weight:600;vertical-align:middle}
 #cats .rnd{font-size:.85rem;font-weight:400;white-space:nowrap;cursor:pointer}
 #cats summary button.ib{color:#fff;background:transparent;border-color:rgba(255,255,255,.75);padding:.15rem .3rem;margin:0}
 #cats summary{gap:.4rem;font-size:1.05rem}
 #cats .cc{white-space:nowrap}
 #cats summary button.ib:hover{background:rgba(255,255,255,.25)}
 #cats details.cat>table,#cats details.cat>.sm{width:calc(100% - 1.5rem);margin:.3rem .75rem .5rem}
 #cats details.cat>.sm{padding:.3rem 0}
 #cats table tr:last-child td{border-bottom:0}
 #cats td.nm{word-break:normal;padding-left:.6rem;border-left:3px solid #cfe2ff;font-size:.95rem;font-weight:400}
 #cats details.un td.nm{border-left-color:#dee2e6}
 #cats td.nm .fn{font-size:.78rem;color:#6c757d;word-break:break-all}
 #cats .sum{font-size:.9rem;margin:.5rem 0}
 #cats .sum.bad{color:#b02a37}
 dialog#swdlg{border:1px solid #dee2e6;border-radius:.5rem;padding:1rem;width:min(92vw,420px);box-sizing:border-box}
 dialog#swdlg::backdrop{background:rgba(0,0,0,.45)}
 #swdlg h3{margin:0 0 .5rem;font-weight:500}
 #swdlg .sm{font-size:.95rem;color:#212529;margin:.4rem 0 .8rem}
 #swdlg .sm p{margin:.4rem 0}
 dialog#edlg{border:1px solid #dee2e6;border-radius:.5rem;padding:1rem;width:min(92vw,420px);box-sizing:border-box}
 dialog#edlg::backdrop{background:rgba(0,0,0,.45)}
 #edlg h3{margin:0 0 .25rem;font-weight:500}
 #edlg .sm{font-size:.8rem;color:#6c757d;margin:.2rem 0}
 #edlg label{display:block;font-weight:600;font-size:.9rem;margin:.6rem 0 .15rem}
 #edlg input[type=text]{width:100%;box-sizing:border-box;padding:.375rem .75rem;border:1px solid #ced4da;border-radius:.375rem;font-size:1rem}
 #edlg .cl label{font-weight:400;margin:.1rem 0;display:block}
 #ederr{color:#b02a37;min-height:1.2em;font-size:.9rem;margin:.4rem 0}
</style></head><body>
<h2>Wi-Fi USB Stick <small id="ver" style="font-size:13px;font-weight:400;color:#888"></small>
 <a id="updi" href="#upd" hidden title="A firmware update is available" aria-label="A firmware update is available"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zm0 5l5 5h-3.5v5h-3v-5H7z"/></svg></a></h2>
<div id="host"></div>
<details id="wifi"><summary>Wi-Fi network (join your router)</summary>
 <div id="wst"></div>
 <div id="wsaved"></div>
 <button id="scan">Scan for networks</button>
 <select id="nets"><option value="">(scan to list networks)</option></select>
 <input type="text" id="ssid" placeholder="Network name (type it here if not listed)" maxlength="32" autocapitalize="off" autocomplete="off">
 <input type="password" id="pass" placeholder="Password (blank if open)" maxlength="63" autocomplete="off">
 <label class="chk"><input type="checkbox" id="showpw"> Show password</label>
 <label class="chk"><input type="checkbox" id="hid"> Hidden / not listed (just save it, don't connect now)</label>
 <div><button id="wconn">Connect</button></div>
</details>
<details id="upd"><summary>Firmware update<span id="updtag"></span></summary>
 <div id="updst" style="margin:6px 0"></div>
 <progress id="updbar" max="100" value="0" hidden></progress>
 <div>
  <button id="updcheck">Check for updates</button>
  <button id="updgo" hidden></button>
 </div>
 <p class="sm">Installing an update keeps your settings, your saved Wi-Fi networks and every file on the card. The new firmware is written next to the running one and the stick restarts into it. Do not unplug the stick while it installs.</p>
 <p class="sm">No internet? Download the file ending in <code>-app.bin</code> from the project's GitHub releases page on a device that has internet, then pick it here:</p>
 <input type="file" id="updfile" accept=".bin">
 <button id="updfilego">Install this file</button>
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
  <li>Upload .wav files to the <b>Sound library</b> (any folder). You can add, play and organise them any time, even while the car is using its drive. Press <b>Play</b> to listen first.</li>
  <li>Press <b>Make Lock Chime</b> on the one you want. It is copied to /LockChime.wav on the car drive and the original stays in the library. The page warns you if the file doesn't look right.</li>
  <li>The stick then <b>restarts</b>, because the car only reads the new sound when the stick looks unplugged and plugged in again. The screen shows each step. Your phone may drop off the stick's Wi-Fi for a few seconds.</li>
  <li><b>Button on the stick:</b> a short press makes a random 16-bit PCM .wav (under the size limit) from the categories marked <b>Random</b> on the Sounds tab the lock chime, then restarts. The <b>Random chime</b> button on the Sounds tab does the same. Holding it for more than 5 seconds only restarts the stick.</li>
 </ul>
 <h4>If the car still plays the old sound</h4>
 <ul>
  <li>Re-open the Lock Sound setting and select USB again. Some owners report the car keeps the old file until they pick another sound and then USB again.</li>
  <li>Check the name, the size, that it is a real PCM WAV, and that it is at the root of the drive.</li>
  <li>Try the other USB port, and make sure Lock Sound is switched on.</li>
 </ul>
 <p class="sm">Do not use this stick as the car's dashcam or Sentry drive: its USB is full speed only, and if the car offers to format the drive, say no (the stick's sound library would survive, but the car drive's contents would not). Software and models differ, and reports from owners don't always agree, so your owner's manual is the final word.</p>
</details>
<div id="bar"><span id="mode"></span><br><button id="reboot" title="Restart the stick (it starts in Car Mode)">Restart stick</button><div id="modehelp"></div></div>
<div id="tabs"><button id="tbb">Boombox Drive</button><button id="tsd">Storage Drive</button></div>
<div id="panel">
<div id="subtabs"><button id="tlib">Sounds</button><button id="tfiles">Library files</button></div>
<div id="volinfo"></div>
<div id="tools">
 <input type="file" id="up" multiple>
 <button id="upb" class="ib"></button>
 <button id="mk" class="ib"></button>
</div>
<div id="path"></div>
<div id="msg"></div>
<div id="cats" style="display:none"></div>
<table id="files"></table>
<dialog id="swdlg"><h3 id="swtitle"></h3>
 <div id="swbody" class="sm"></div>
 <div><button type="button" class="bb bb-primary" id="swgo">Switch</button><button type="button" class="bb" id="swcancel">Cancel</button></div>
</dialog>
<dialog id="edlg"><h3>Edit sound</h3>
 <label for="edname">Name</label><input type="text" id="edname" maxlength="60" autocomplete="off">
 <label for="edfile">File name</label><input type="text" id="edfile" maxlength="64" autocomplete="off" autocapitalize="off" spellcheck="false">
 <div class="sm">Letters, numbers, spaces and _ - . ( ) &amp; ' # only, ending in .wav</div>
 <label>Categories</label><div class="cl" id="edcats"></div>
 <div class="sm">A sound in no category is in UnCategorized.</div>
 <div id="ederr"></div>
 <div><button type="button" class="bb bb-primary" id="edsave">Save</button><button type="button" class="bb" id="edcancel">Cancel</button></div>
</dialog>
<div id="legend"></div>
<details id="card"><summary>Car drive tools</summary>
 <p class="sm">If the car's drive is damaged or the car stops reading it, you can erase and re-create it. Only the 4 GB "Boombox" drive is erased; your sound library is kept and the last lock chime is put back. The car sees its drive disappear for about a minute.</p>
 <button id="rebuild">Erase and re-create the Boombox drive</button>
</details>
</div>
<script>
let dir='/',usb=false,pcLib=false,vol='car',hasStore=false,dirs={store:'/',car:'/'},lview='cats',lib=null;
const IC={
 play:'M8 5v14l11-7z',stop:'M6 6h12v12H6z',
 chime:'M12 22a2 2 0 0 0 2-2h-4a2 2 0 0 0 2 2zm6-6v-5a6 6 0 0 0-4.5-5.8V4.5a1.5 1.5 0 0 0-3 0v.7A6 6 0 0 0 6 11v5l-2 2v1h16v-1z',
 rename:'M3 17.25V21h3.75L17.8 9.94l-3.75-3.75zM20.7 7a1 1 0 0 0 0-1.4l-2.3-2.3a1 1 0 0 0-1.4 0l-1.8 1.8 3.75 3.75z',
 del:'M6 19a2 2 0 0 0 2 2h8a2 2 0 0 0 2-2V7H6zM19 4h-3.5l-1-1h-5l-1 1H5v2h14z',
 upload:'M9 16h6v-6h4l-7-7-7 7h4zm-4 2h14v2H5z',
 folder:'M20 6h-8l-2-2H4a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h16a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2zm-1 8h-3v3h-2v-3h-3v-2h3V9h2v3h3z'};
const svg=k=>'<svg class="ic" viewBox="0 0 24 24" aria-hidden="true"><path d="'+IC[k]+'"/></svg>';
function setIcon(b,k,label){b.className='ib';b.innerHTML=svg(k);b.title=label;b.setAttribute('aria-label',label)}
function ibtn(k,label,fn){const b=document.createElement('button');setIcon(b,k,label);b.onclick=fn;return b}
const V=()=>'&vol='+vol;
const $=s=>document.querySelector(s);
const enc=encodeURIComponent;
const join=(d,n)=>(d==='/'?'':d)+'/'+n;
const msg=t=>{$('#msg').textContent=t||''};
async function api(u,o){const r=await fetch(u,o);if(!r.ok)throw new Error(await r.text());return r}
function fmt(n){return n<1024?n+' B':n<1048576?(n/1024).toFixed(1)+' KB':n<1073741824?(n/1048576).toFixed(1)+' MB':(n/1073741824).toFixed(1)+' GB'}

async function load(){
 stopPlay();
 msg();
 wstatus();
 const st=await (await fetch('/api/status')).json();
 usb=!!st.usb;pcLib=!!st.pcLib;
 hasStore=!!st.store;
 vol=usb?'store':'car';dir=dirs[vol];             // the tab always follows the mode: Storage Drive = Car Mode, Boombox Drive = PC Mode
 $('#bar').className=usb?'usb':'';
 $('#mode').textContent=st.problem?'Card problem: '+st.problem+'.':usb
  ?'Storage Drive tab (Car Mode): the car has the Boombox drive. You edit your sounds here.'
  :'Boombox Drive tab (PC Mode): the stick has the Boombox drive, so you can edit it here. '+(pcLib?'A computer on the USB port sees your sound library as a drive.':'');
 if(st.ver)$('#ver').textContent='v'+st.ver;
 if(st.host)$('#host').textContent='Name on your network: '+st.host+'  (http://'+st.host+'.local)';
 updLoad();
 $('#modehelp').textContent='Tap the other tab to switch modes. The stick always starts in Storage Drive (Car Mode).';
 const catsView=vol==='store'&&lview==='cats';
 $('#tsd').className=vol==='store'?'on':'';
 $('#tbb').className=vol==='car'?'on':'';
 $('#subtabs').style.display=vol==='store'&&hasStore?'':'none';
 $('#tlib').className=catsView?'on':'';
 $('#tfiles').className=vol==='store'&&!catsView?'on':'';
 $('#card').style.display=vol==='car'?'':'none';       // re-creating the Boombox drive belongs to the PC Mode tab
 const editable=vol==='store'?hasStore:true;
 $('#volinfo').textContent=vol==='store'
  ?(hasStore?'Your sounds, on the stick\'s own storage. The car never sees these files.'+(st.stTotal?' '+fmt(st.stFree*1024)+' free of '+fmt(st.stTotal*1024)+'.':''):'The storage drive is not available, so it can\'t be edited.')
  :'What the car sees when the stick is in Storage Drive mode. Make Lock Chime writes LockChime.wav here.'+(st.total?' '+fmt(st.used)+' used of '+fmt(st.total)+'.':'');
 $('#tools').style.display=editable?'':'none';
 $('#mk').style.display=catsView?'none':'';
 $('#path').textContent=editable&&!catsView?(hasStore?(vol==='store'?'Library':'Car drive')+': ':'')+dir:'';
 $('#cats').style.display=catsView&&editable?'':'none';
 $('#files').style.display=catsView?'none':'';
 const t=$('#files');t.innerHTML='';
 if(!editable)return;
 if(catsView){await catsLoad();return}
 let list;
 try{list=await (await api('/api/list?dir='+enc(dir)+V())).json()}catch(e){msg(e.message);return}
 list.sort((a,b)=>b.d-a.d||a.n.localeCompare(b.n));
 if(dir!=='/'){
  const r=t.insertRow();const c=r.insertCell();c.colSpan=3;
  const a=document.createElement('a');a.textContent='.. (up)';
  a.onclick=()=>{dir=dirs[vol]=dir.substring(0,dir.lastIndexOf('/'))||'/';load()};
  c.appendChild(a);
 }
 for(const f of list){
  const r=t.insertRow();
  const c=r.insertCell();const a=document.createElement('a');
  if(f.d){a.textContent='[ '+f.n+' ]';a.onclick=()=>{dir=dirs[vol]=join(dir,f.n);load()}}
  else{a.textContent=f.n;a.href='/download?path='+enc(join(dir,f.n))+V()}
  c.appendChild(a);
  const s=r.insertCell();s.className='s';s.textContent=f.d?'':fmt(f.s);
  const x=r.insertCell();x.className='a';
  if(!f.d&&/\.wav$/i.test(f.n)){
   x.appendChild(playBtn(join(dir,f.n)));
   if(vol==='store'||!(dir==='/'&&f.n.toLowerCase()==='lockchime.wav'))
   x.appendChild(ibtn('chime','Make Lock Chime',()=>chimeFlow(join(dir,f.n),f.n)));
  }
  x.appendChild(ibtn('rename','Rename',async()=>{
   const n=prompt('New name',f.n);
   if(!n||n===f.n)return;
   try{await api('/rename?from='+enc(join(dir,f.n))+'&to='+enc(join(dir,n))+V(),{method:'POST'});load()}catch(e){msg(e.message)}
  }));
  x.appendChild(ibtn('del','Delete',async()=>{
   if(!confirm('Delete '+f.n+'?'))return;
   try{await api('/delete?path='+enc(join(dir,f.n))+V(),{method:'POST'});load()}catch(e){msg(e.message)}
  }));
 }
}

async function chimeFlow(path,n){
 let m='Copy '+n+' to LockChime.wav on the car drive?\n(An existing LockChime.wav will be replaced.'+' The stick restarts afterwards so the car reads the new sound.'+')';
 try{
  const i=await (await api('/api/wavinfo?path='+enc(path)+V())).json();
  if(i.ok)m+='\n\n'+(i.ch==1?'Mono':i.ch==2?'Stereo':i.ch+' channels')+', '+i.bits+'-bit, '+i.rate+' Hz, '+i.secs.toFixed(1)+' s, '+fmt(i.size);
  if(i.warn.length)m+='\n\nWarnings:\n- '+i.warn.join('\n- ')+'\n\nThe car may not play this file. Copy it anyway?';
 }catch(e){}
 if(!confirm(m))return;
 msg('Copying...');
 try{await api('/lockchime?path='+enc(path)+V(),{method:'POST'});afterChime(n)}catch(e){msg(e.message)}
}

// ---- Sounds tab: categories ----
const bit=(m,i)=>((m>>>i)&1)===1;
const postForm=(u,o)=>api(u,{method:'POST',body:new URLSearchParams(o)});
async function catsLoad(){
 try{lib=await (await api('/api/lib')).json()}catch(e){msg(e.message);$('#cats').innerHTML='';return}
 catsDraw();
}
function randomCount(){
 const on=lib.cats.filter(c=>c.r).map(c=>c.i);
 return lib.sounds.filter(s=>s.c===0?on.includes(0):on.some(i=>i>0&&bit(s.c,i))).length;
}
function catsDraw(){
 const box=$('#cats');box.innerHTML='';
 const top=document.createElement('div');
 const nb=document.createElement('button');nb.className='bb bb-primary';nb.textContent='New category';nb.onclick=newCat;
 top.appendChild(nb);
 const rb=document.createElement('button');rb.className='bb bb-success';rb.textContent='Random chime';
 rb.title='Pick a random sound from the categories marked Random, like the button on the stick';rb.onclick=randomChime;
 top.appendChild(rb);box.appendChild(top);
 const sum=document.createElement('div');box.appendChild(sum);
 const sumUpd=()=>{const n=randomCount();sum.className='sum'+(n?'':' bad');
  sum.textContent=n?'The stick\'s button picks a random chime from the '+n+' sound'+(n==1?'':'s')+' in the categories marked Random.'
   :'No sound is in a category marked Random, so the stick\'s button has nothing to pick. Tick Random on at least one category.'};
 sumUpd();
 const cats=[lib.cats[0]].concat(lib.cats.slice(1).sort((a,b)=>a.n.localeCompare(b.n)));
 for(const c of cats){
  const items=lib.sounds.filter(s=>c.i===0?s.c===0:bit(s.c,c.i)).sort((a,b)=>a.n.localeCompare(b.n));
  const d=document.createElement('details');d.className='cat'+(c.i===0?' un':'');d.open=true;
  const sm=document.createElement('summary');
  const nm=document.createElement('span');nm.className='cn';nm.textContent=c.n+' ';
  const cc=document.createElement('span');cc.className='cc';cc.textContent='('+items.length+')';nm.appendChild(cc);
  sm.appendChild(nm);
  const lab=document.createElement('label');lab.className='rnd';
  const cb=document.createElement('input');cb.type='checkbox';cb.checked=!!c.r;
  cb.onchange=async()=>{
   try{await postForm('/lib/cat/rand',{id:c.i,on:cb.checked?1:0});c.r=cb.checked?1:0;sumUpd()}
   catch(e){cb.checked=!cb.checked;msg(e.message)}
  };
  lab.appendChild(cb);lab.appendChild(document.createTextNode(' Random'));sm.appendChild(lab);
  if(c.i>0){
   sm.appendChild(ibtn('rename','Rename this category',e=>{e.preventDefault();renameCat(c)}));
   sm.appendChild(ibtn('del','Delete this category (the sounds stay)',e=>{e.preventDefault();deleteCat(c)}));
  }
  d.appendChild(sm);
  if(!items.length){const p=document.createElement('div');p.className='sm';p.textContent='No sounds here.';d.appendChild(p)}
  const t=document.createElement('table');
  for(const s of items){
   const r=t.insertRow();
   const n=r.insertCell();n.className='nm';n.textContent=s.n;
   const fn=document.createElement('div');fn.className='fn';fn.textContent=s.f.replace(/^\//,'');n.appendChild(fn);
   const x=r.insertCell();x.className='a';
   x.appendChild(playBtn(s.f));
   x.appendChild(ibtn('chime','Make Lock Chime',()=>chimeFlow(s.f,s.n)));
   x.appendChild(ibtn('rename','Edit name, file name and categories',()=>editSound(s)));
   x.appendChild(ibtn('del','Delete the file',async()=>{
    if(!confirm('Delete "'+s.n+'" ('+s.f.replace(/^\//,'')+') from the library?'))return;
    try{await api('/delete?path='+enc(s.f)+V(),{method:'POST'});catsLoad()}catch(e){msg(e.message)}
   }));
  }
  d.appendChild(t);box.appendChild(d);
 }
}
async function randomChime(){
 if(!randomCount()){alert('No sound is in a category marked Random. Tick Random on at least one category first.');return}
 if(!confirm('Pick a random sound from the categories marked Random and make it the lock chime?\n\nThis does what a short press of the stick\'s button does: the stick restarts afterwards so the car reads the new sound.'))return;
 stopPlay();msg('Picking...');
 try{const r=await api('/random',{method:'POST'});afterChime(await r.text())}catch(e){msg(e.message)}
}
async function newCat(){
 const n=prompt('Category name (for example Christmas Sounds)');if(!n)return;
 try{await postForm('/lib/cat/add',{name:n});catsLoad()}catch(e){msg(e.message)}
}
async function renameCat(c){
 const n=prompt('New category name',c.n);if(!n||n===c.n)return;
 try{await postForm('/lib/cat/rename',{id:c.i,name:n});catsLoad()}catch(e){msg(e.message)}
}
async function deleteCat(c){
 if(!confirm('Delete the category "'+c.n+'"?\n\nThe sounds stay in the library. They only leave this category, and a sound that is then in no category goes to UnCategorized.'))return;
 try{await postForm('/lib/cat/delete',{id:c.i});catsLoad()}catch(e){msg(e.message)}
}
function fileNameProblem(f){
 if(f.length>64)return 'The file name is too long (64 characters at most).';
 if(f.length<5||!/\.wav$/i.test(f))return 'The file name must end in .wav';
 const b=f.slice(0,-4);
 if(/^ | $/.test(b))return 'The file name can\'t start or end with a space.';
 if(/^\.|\.$/.test(b))return 'The file name can\'t start or end with a dot.';
 if(b.indexOf('..')>=0)return 'The file name can\'t contain two dots in a row.';
 if(!/^[A-Za-z0-9 _.()&'#-]+$/.test(b))return 'The file name can only use letters, numbers, spaces and _ - . ( ) & \' #';
 return '';
}
// Enter saves the edit dialog (Escape closes it by itself) and joins a Wi-Fi network from its text boxes.
$('#edlg').addEventListener('keydown',e=>{if(e.key==='Enter'&&e.target.tagName!=='BUTTON'){e.preventDefault();$('#edsave').click()}});
for(const id of ['#ssid','#pass'])$(id).addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();$('#wconn').click()}});
function editSound(s){
 const cur=s.f.replace(/^.*\//,'');
 $('#edname').value=s.n;$('#edfile').value=cur;$('#ederr').textContent='';
 const box=$('#edcats');box.innerHTML='';
 const user=lib.cats.filter(c=>c.i>0).sort((a,b)=>a.n.localeCompare(b.n));
 if(!user.length)box.innerHTML='<span class="sm">No categories yet. Close this and press New category.</span>';
 for(const c of user){
  const l=document.createElement('label');const cb=document.createElement('input');cb.type='checkbox';cb.value=c.i;cb.checked=bit(s.c,c.i);
  l.appendChild(cb);l.appendChild(document.createTextNode(' '+c.n));box.appendChild(l);
 }
 $('#edcancel').onclick=()=>$('#edlg').close();
 $('#edsave').onclick=async()=>{
  const name=$('#edname').value.trim(),file=$('#edfile').value.trim();
  if(!name){$('#ederr').textContent='Give the sound a name.';return}
  if(file!==cur){const p=fileNameProblem(file);if(p){$('#ederr').textContent=p;return}}
  let m=0;box.querySelectorAll('input:checked').forEach(cb=>{m+=Math.pow(2,+cb.value)});
  try{await postForm('/lib/sound/set',{path:s.f,name:name,file:file,mask:m});$('#edlg').close();catsLoad()}
  catch(e){$('#ederr').textContent=e.message}
 };
 $('#edlg').showModal();
}

function afterChime(n){
 const t='LockChime.wav is now '+n+'. The stick is restarting so the car reads it; this page comes back by itself (rejoin the stick\'s Wi-Fi if your phone dropped it).';
 $('#files').innerHTML='';$('#tools').style.display='none';msg(t);
 const w=()=>fetch('/api/status').then(r=>r.json()).then(()=>load().then(()=>msg('LockChime.wav is now '+n+'.'))).catch(()=>{msg(t);setTimeout(w,2000)});
 setTimeout(w,6000);
}
$('#tlib').onclick=()=>{vol='store';lview='cats';load()};
$('#tfiles').onclick=()=>{vol='store';lview='files';dir=dirs.store;load()};
function askSwitch(toPc){
 if(toPc===!usb)return;                    // already in that mode
 $('#swtitle').textContent=toPc?'Switch to Boombox Drive?':'Switch to Storage Drive?';
 $('#swbody').innerHTML=toPc
  ?'<p>This switches the stick to <b>PC Mode</b>.</p><p>The <b>car loses the Boombox drive</b>: it sees the drive eject, and the stick takes it so you can add, rename or delete its files here.</p><p>A computer on the USB port is shown your sound library instead. Your files are safe, but Tesla will not play a lock chime until you switch back.</p>'
  :'<p>This switches the stick to <b>Car Mode</b>.</p><p>The <b>car gets the Boombox drive back</b> (it sees a drive being plugged in) and your sound library is hidden from any computer.</p><p>You will edit your sounds on the Storage Drive tab. Do not switch while the car is writing to the drive.</p>';
 $('#swgo').onclick=async()=>{
  $('#swdlg').close();
  msg('Switching...');
  try{await api('/mode?usb='+(toPc?0:1),{method:'POST'})}catch(e){msg(e.message)}
  load();
 };
 $('#swdlg').showModal();
}
$('#swcancel').onclick=()=>$('#swdlg').close();
$('#tbb').onclick=()=>askSwitch(true);
$('#tsd').onclick=()=>askSwitch(false);
$('#reboot').onclick=async()=>{
 if(!confirm('Restart the stick?\n\nThe car sees it unplugged and plugged in again, and the stick starts in Car Mode. This page comes back by itself.'))return;
 try{await api('/reboot',{method:'POST'})}catch(e){msg(e.message);return}
 const t='The stick is restarting. This page comes back by itself (rejoin the stick\'s Wi-Fi if your phone dropped it).';
 $('#files').innerHTML='';$('#tools').style.display='none';msg(t);
 const w=()=>fetch('/api/status',{cache:'no-store'}).then(r=>{if(!r.ok)throw 0;location.reload()}).catch(()=>{msg(t);setTimeout(w,2000)});
 setTimeout(w,6000);
};
setIcon($('#upb'),'upload','Upload files');
setIcon($('#mk'),'folder','New folder');
$('#upb').onclick=()=>$('#up').click();
$('#showpw').onchange=e=>{$('#pass').type=e.target.checked?'text':'password'};
$('#legend').innerHTML=[['upload','upload'],['folder','new folder'],['play','play'],['chime','make lock chime'],['rename','rename'],['del','delete']].map(a=>'<span>'+svg(a[0])+' '+a[1]+'</span>').join('');
$('#mk').onclick=async()=>{
 const n=prompt('Folder name');if(!n)return;
 try{await api('/mkdir?path='+enc(join(dir,n))+V(),{method:'POST'});load()}catch(e){msg(e.message)}
};
$('#up').onchange=async e=>{
 const d=vol==='store'&&lview==='cats'?'/':dir;      // new sounds go to the top of the library; they start as UnCategorized
 for(const f of e.target.files){
  msg('Uploading '+f.name+'...');
  const fd=new FormData();fd.append('file',f,f.name);
  try{await api('/upload?dir='+enc(d)+V(),{method:'POST',body:fd})}catch(err){msg(err.message);break}
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
 const save=!force&&$('#hid').checked;
 $('#wst').textContent=save?'Saving...':'Checking network...';
 const body=new URLSearchParams({ssid:$('#ssid').value,pass:$('#pass').value,hidden:save?'1':'0',force:force?'1':'0'});
 const r=await fetch('/wifi/connect',{method:'POST',body});
 const t=await r.text();
 if(r.status===404&&!force){
  if(confirm('"'+$('#ssid').value+'" was not found in a scan (it may be hidden or out of range). Try connecting anyway?'))return wconnect(true);
 }
 $('#wst').textContent=t;
 if(r.ok&&save){wsaved();return}
 if(r.ok&&!wpoll)wpoll=setInterval(wstatus,2000);
}
$('#wconn').onclick=()=>wconnect(false).catch(e=>{$('#wst').textContent=e.message});

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
   row.appendChild(ibtn('del','Delete this saved network',async()=>{
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
 if(playBtnEl){setIcon(playBtnEl,'play','Play');playBtnEl=null}
}
function playBtn(path){
 const b=ibtn('play','Play',()=>{
  if(playBtnEl===b){stopPlay();return}
  stopPlay();
  player=new Audio('/play?path='+enc(path)+V());
  playBtnEl=b;setIcon(b,'stop','Stop');
  player.onended=stopPlay;
  player.onerror=()=>{stopPlay();msg('Could not play this file. Is it a valid WAV?')};
  player.play().catch(()=>{stopPlay()});
 });
 return b;
}

// ---- firmware update ----
let upd={cur:'',latest:'',avail:0,net:0,state:'idle',pct:0,err:'',base:''},updWeb=null,updWebTried=false,updTimer=0,updBusy=false;
const vnew=(a,b)=>{const x=String(a).split(/\D+/).map(Number),y=String(b).split(/\D+/).map(Number);
 for(let i=0;i<Math.max(x.length,y.length);i++){const p=x[i]||0,q=y[i]||0;if(p!==q)return p>q}return false};
// the newest version either the stick or this device's own internet connection knows about
function updNewest(){
 let v='',via='';
 if(upd.avail&&upd.latest){v=upd.latest;via='stick'}
 if(updWeb&&vnew(updWeb.ver,upd.cur)&&(!v||vnew(updWeb.ver,v))){v=updWeb.ver;via='web'}
 return {v,via};
}
async function updWebCheck(){
 if(!upd.base)return;
 try{
  const c=new AbortController();setTimeout(()=>c.abort(),6000);
  const j=await (await fetch(upd.base+'latest.json',{cache:'no-store',signal:c.signal})).json();
  if(j&&j.version&&j.file)updWeb={ver:String(j.version),url:upd.base+j.file};
 }catch(e){}
}
async function updLoad(){
 if(updBusy)return;
 try{upd=await (await fetch('/api/update',{cache:'no-store'})).json()}catch(e){return}
 if(!updWebTried){updWebTried=true;await updWebCheck()}
 updDraw();
}
function updDraw(){
 const n=updNewest();
 $('#updi').hidden=!n.v;
 $('#updtag').textContent=n.v?' - v'+n.v+' available':'';
 let t='Installed: v'+upd.cur+'. ';
 if(n.v)t+='New version: v'+n.v+'.';
 else if(upd.latest||updWeb)t+='This is the latest version.';
 else t+='Not checked yet.'+(upd.net?'':' (The stick is not on a Wi-Fi network with internet, so it can\'t look by itself.)');
 if(upd.err&&!updBusy)t+=' '+upd.err;
 $('#updst').textContent=t;
 const g=$('#updgo');
 g.hidden=!n.v||updBusy;
 g.textContent='Install v'+n.v;
 $('#updcheck').disabled=updBusy;
}
function updProgress(txt,pct){
 $('#updst').textContent=txt;
 const b=$('#updbar');b.hidden=false;
 if(pct===null)b.removeAttribute('value');else b.value=pct;
}
function updWaitReboot(){
 updProgress('Restarting the stick. This page reloads by itself (rejoin the stick\'s Wi-Fi if your phone dropped it)...',null);
 const w=()=>fetch('/api/status',{cache:'no-store'}).then(r=>{if(!r.ok)throw 0;location.reload()}).catch(()=>setTimeout(w,2000));
 setTimeout(w,6000);
}
function updWatch(){
 clearInterval(updTimer);
 updTimer=setInterval(async()=>{
  let s;
  try{s=await (await fetch('/api/update',{cache:'no-store'})).json()}catch(e){clearInterval(updTimer);updWaitReboot();return}
  upd=s;
  if(s.state==='error'){clearInterval(updTimer);updBusy=false;$('#updbar').hidden=true;updDraw();return}
  if(s.state==='done'){clearInterval(updTimer);updWaitReboot();return}
  updProgress(s.state==='downloading'?'The stick is downloading and installing the update: '+s.pct+'%':'Working...',s.state==='downloading'?s.pct:null);
 },1000);
}
function updUpload(blob,name){
 return new Promise((res,rej)=>{
  const x=new XMLHttpRequest();x.open('POST','/update/upload');
  x.upload.onprogress=e=>{if(e.lengthComputable)updProgress('Sending the update to the stick: '+Math.round(e.loaded*100/e.total)+'%',e.loaded*100/e.total)};
  x.onload=()=>x.status===200?res():rej(new Error(x.responseText||('Error '+x.status)));
  x.onerror=()=>rej(new Error('The upload failed. Are you still on the stick\'s Wi-Fi?'));
  const f=new FormData();f.append('fw',blob,name);x.send(f);
 });
}
async function updRun(fn){
 updBusy=true;$('#updgo').hidden=true;$('#updcheck').disabled=true;
 try{await fn()}
 catch(e){updBusy=false;$('#updbar').hidden=true;await updLoad();$('#updst').textContent='Update failed: '+e.message+' Nothing was changed.'}
}
const updWarn='\n\nYour settings, saved Wi-Fi networks and files are kept. The stick restarts afterwards; do not unplug it while it installs.';
$('#updcheck').onclick=async()=>{
 $('#updst').textContent='Checking...';
 let ok=false;
 if(upd.net){
  try{await api('/update/check',{method:'POST'});
   for(let i=0;i<20;i++){await new Promise(r=>setTimeout(r,1000));upd=await (await fetch('/api/update',{cache:'no-store'})).json();if(upd.state!=='checking')break}
   ok=!upd.err}catch(e){}
 }
 updWebTried=true;await updWebCheck();
 updDraw();
 if(!ok&&!updWeb&&!upd.latest)$('#updst').textContent='Could not reach GitHub, neither from the stick nor from this device.'+(upd.err?' '+upd.err:'')+' Join your home Wi-Fi in the "Wi-Fi network" panel, or install a downloaded file below.';
};
$('#updgo').onclick=()=>{
 const n=updNewest();if(!n.v)return;
 if(!confirm('Install firmware v'+n.v+'?'+updWarn))return;
 updRun(async()=>{
  if(n.via==='stick'){
   updProgress('Starting...',null);
   await api('/update/install',{method:'POST'});
   updWatch();
  }else{
   updProgress('Downloading v'+n.v+' from GitHub...',null);
   const r=await fetch(updWeb.url,{cache:'no-store'});
   if(!r.ok)throw new Error('GitHub answered '+r.status+'.');
   const blob=await r.blob();
   await updUpload(blob,'firmware-app.bin');
   updWaitReboot();
  }
 });
};
$('#updfilego').onclick=()=>{
 const f=$('#updfile').files[0];
 if(!f){alert('Choose the firmware file (...-app.bin) first.');return}
 if(!confirm('Install '+f.name+' ('+fmt(f.size)+')?'+updWarn))return;
 updRun(async()=>{await updUpload(f,f.name);updWaitReboot()});
};
if(location.hash==='#upd')$('#upd').open=true;
$('#updi').onclick=()=>{$('#upd').open=true};

$('#rebuild').onclick=async()=>{
 if(!confirm('Erase everything on the car\'s Boombox drive and re-create it?\n\nYour sound library is kept and the last lock chime is put back.'))return;
 try{
  await api('/card/rebuild',{method:'POST',body:new URLSearchParams({confirm:'ERASE'})});
  msg('Re-creating the Boombox drive. This takes about a minute; the page will not respond until it is done.');
  setTimeout(load,3000);
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
  out += ",\"host\":\"" + hostName + "\"";
  if (updAvail) out += ",\"upd\":\"" + jsonEscape(updLatest) + "\"";     // a newer firmware is published
  if (cardProblem.length()) out += ",\"problem\":\"" + jsonEscape(cardProblem) + "\"";
  out += ",\"store\":" + String(storeMounted ? 1 : 0);
  out += ",\"pcLib\":" + String(hostView == HOST_STORE ? 1 : 0);   // a computer has the library
  uint64_t stTotal = 0, stFree = 0;
  if (STORE.space(stTotal, stFree)) {
    out += ",\"stTotal\":" + String((unsigned long)(stTotal / 1024));   // KB, fits in 32 bits
    out += ",\"stFree\":" + String((unsigned long)(stFree / 1024));
  }
  out += "}";
  server.send(200, "application/json", out);
}

// ---------------- First Run ----------------
// Every card must have the two-partition layout (see CARD LAYOUT). A card that
// doesn't (new, exFAT, used somewhere else) is erased and set up automatically:
// with a screen, a countdown (unplug the stick to cancel); without one, the stick
// waits until the BOOT button is held down. Then the format, then a restart. Until then the car is not given a drive, and the web server only
// serves a small progress page, so nothing else can touch the card.
#define FIRST_RUN_WAIT_MS 15000         // with a screen: countdown before erasing
#define FIRST_RUN_HOLD_MS 5000          // without a screen: hold the BOOT button this long
const char FIRSTRUN_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Wi-Fi USB Stick - First Run</title>
<style>
 body{font:16px system-ui,sans-serif;margin:0 auto;padding:12px;max-width:720px}
 #box{padding:14px;border-radius:8px;background:#fed}
 #bar{height:14px;border-radius:7px;background:#ddd;margin:12px 0;overflow:hidden}
 #fill{height:100%;width:0;background:#06c}
 .sm{font-size:13px;color:#666}
</style></head><body>
<h2>Wi-Fi USB Stick <small id="ver" style="font-size:13px;font-weight:400;color:#888"></small></h2>
<div id="box"><b>First Run: setting up the SD card</b>
 <div id="txt">Checking...</div>
 <div id="bar"><div id="fill"></div></div>
 <div class="sm">The card is erased and split into a 4 GB "Boombox" drive for the car and a "CHIME_STORE" sound library for the stick. Do not unplug the stick once this has started. Formatting takes a few minutes. The stick restarts when done; this page then reloads by itself (rejoin the stick's Wi-Fi if your phone dropped it).</div>
 <div class="sm" id="noscr" style="display:none;margin-top:8px"><b>No screen on your stick?</b> It will not start by itself. Hold the button on the stick down for more than 5 seconds, then let go. When it is plugged into a computer, wait until the stick shows up as a USB drive on the computer before you unplug it: that is the sign that setup has finished.</div>
</div>
<script>
const $=s=>document.querySelector(s);
async function tick(){
 try{
  const st=await (await fetch('/api/status')).json();
  if(!st.firstRun){location.reload();return}
  if(st.ver)$('#ver').textContent='v'+st.ver;
  $('#fill').style.width=st.pct+'%';
  $('#noscr').style.display=st.hold?'':'none';
  $('#txt').textContent=st.err?'Card setup failed: '+st.err+'. Unplug the stick, check or replace the card, and plug it in again.'
   :st.hold?'This card ('+(st.cardMB/1024).toFixed(1)+' GB) is not set up yet. Hold the button on the stick for more than 5 seconds to erase it and set it up.'
   :st.wait?'This card ('+(st.cardMB/1024).toFixed(1)+' GB) is not set up yet. Erasing it in '+st.wait+' s. Unplug the stick now to keep what is on the card.'
   :'Formatting: '+st.pct+'% done.';
 }catch(e){$('#txt').textContent='Waiting for the stick...'}
 setTimeout(tick,1500);
}
tick();
</script></body></html>)HTML";

String firstRunErr;
uint32_t firstRunAt = 0;               // when the countdown started
bool firstRunDone = false;             // the format was attempted (and failed, or the stick would have restarted)

static uint32_t firstRunWait() {       // seconds left on the countdown
  if (!cfgDisplay) return 0;           // no screen: started with the button instead
  uint32_t el = millis() - firstRunAt;
  return (firstRunDone || el >= FIRST_RUN_WAIT_MS) ? 0 : (FIRST_RUN_WAIT_MS - el + 999) / 1000;
}

void handleFirstRunRoot() {
  server.send_P(200, "text/html", FIRSTRUN_HTML);
}

void handleFirstRunStatus() {
  String out = "{\"firstRun\":1,\"ver\":\"" FW_VERSION "\"";
  out += ",\"cardMB\":" + String((unsigned long)(layout.total / 2048));
  out += ",\"wait\":" + String((unsigned long)firstRunWait());
  out += ",\"hold\":" + String((!cfgDisplay && !firstRunDone) ? 1 : 0);   // waiting for the button
  out += ",\"pct\":" + String(fmtPercent);
  out += ",\"err\":\"" + jsonEscape(firstRunErr) + "\"}";
  server.send(200, "application/json", out);
}

// Called from loop() while firstRun is set.
void firstRunTick() {
  if (firstRunDone) return;
  if (!cfgDisplay) {
    // Nobody can see a countdown, so nothing is erased until the button is held.
    static uint32_t downSince = 0;
    if (digitalRead(BUTTON_PIN) != LOW) { downSince = 0; return; }
    if (downSince == 0) downSince = millis() | 1;
    if (millis() - downSince <= FIRST_RUN_HOLD_MS) return;
  } else {
    uint32_t wait = firstRunWait();
    if (wait > 0) {
      static uint32_t shown = 0;
      if (wait != shown) {
        shown = wait;
        showBig("First Run", "Erasing card in", String((unsigned long)wait) + " s", "Unplug to cancel", C_ORANGE, 5000);
      }
      return;
    }
  }
  firstRunDone = true;
  xSemaphoreTake(sdLock, portMAX_DELAY);
  formatting = true;
  const char* err = formatSplitCard(layout.total, esp_random());
  formatting = false;
  xSemaphoreGive(sdLock);
  if (err[0]) {                        // stay up and say so (no restart: that would only try again)
    firstRunErr = err;
    cardProblem = "Card setup failed";
    showToast("Setup failed", err, 15000, false);
    return;
  }
  showBig("First Run", "Card is ready", "OK", "Restarting...", C_GREEN, 5000);
  uint32_t t = millis();
  while (millis() - t < 2500) { server.handleClient(); delay(5); }   // let the page see 100%
  ESP.restart();                       // start fresh with the new layout
}

// ---------------- Re-create the car's drive ----------------
// Formats only the Boombox partition and puts the last lock chime back. Used
// from the web page, and at start-up when the car's drive can't be mounted.
bool rebuildRequested = false;
void handleCardRebuild() {
  if (!layout.split) { server.send(409, "text/plain", "No usable SD card."); return; }
  if (server.arg("confirm") != "ERASE") { server.send(400, "text/plain", "Not confirmed."); return; }
  rebuildRequested = true;             // the work runs from loop()
  server.send(200, "text/plain", "OK");
}

void doCardRebuild() {
  rebuildRequested = false;
  if (uploadFile) uploadFile.close();
  takeStoreBack();                     // the chime copy is in the library
  if (usbMode) setUsbMode(false);      // the car sees its drive eject
  xSemaphoreTake(sdLock, portMAX_DELAY);
  formatting = true;
  const char* err = formatBoombox(esp_random());
  formatting = false;
  SD_MMC.end();                        // forget the old file system
  bool mounted = mountSD();
  xSemaphoreGive(sdLock);
  if (err[0] || !mounted) {
    cardProblem = "Car drive unreadable";
    showToast("Rebuild failed", err[0] ? err : "Could not mount the new drive", 15000, false);
    return;
  }
  cardProblem = "";
  sectorCount = cardSectors();
  cardTotal = SD_MMC.totalBytes();
  bool chime = restoreLockChime();
  setUsbMode(true);                    // names the drive and gives it back to the car
  showToast("Drive ready", chime ? "Lock chime put back" : "No lock chime to put back", 4000, true);
}

void handleList() {
  if (!requireVol()) return;
  String dir = safePath(server.arg("dir"));
  File root = volFS().open(dir);
  if (!root || !root.isDirectory()) {
    server.send(404, "text/plain", "Folder not found");
    return;
  }
  String out = "[";
  bool first = true;
  File f = root.openNextFile();
  while (f) {
    if (String(f.name()) == (CHIME_BACKUP + 1) || String(f.name()).startsWith(".sounds.")) { f = root.openNextFile(); continue; }   // the stick's own files
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
  if (!requireVol()) return;
  String path = safePath(server.arg("path"));
  File f = volFS().open(path);
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
  if (updRunning || (isStoreVol() ? !storeMounted : usbMode)) return;
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    String dir = safePath(server.arg("dir"));
    String name = up.filename;
    name.replace("/", "_");
    name.replace("\\", "_");
    if (isStoreVol()) { name = libSafeUploadName(name); libStale = true; }   // the library only allows tidy file names
    String path = (dir == "/" ? "" : dir) + "/" + name;
    if (uploadFile) uploadFile.close();
    uploadFile = volFS().open(path, FILE_WRITE);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) uploadFile.write(up.buf, up.currentSize);
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
  }
}

void handleUploadDone() {
  if (!requireVol()) return;
  server.send(200, "text/plain", "OK");
}

void handleRename() {
  if (!requireVol()) return;
  String from = safePath(server.arg("from"));
  String to = safePath(server.arg("to"));
  if (from == "/" || to == "/") { server.send(400, "text/plain", "Bad name"); return; }
  if (volFS().exists(to)) { server.send(409, "text/plain", "A file with that name already exists"); return; }
  if (volFS().rename(from, to)) {
    if (isStoreVol()) libPathMoved(from, to);
    server.send(200, "text/plain", "OK");
  } else server.send(500, "text/plain", "Rename failed");
}

void handleDelete() {
  if (!requireVol()) return;
  String path = safePath(server.arg("path"));
  if (path == "/") { server.send(400, "text/plain", "Bad path"); return; }
  File f = volFS().open(path);
  bool isDir = f && f.isDirectory();
  if (f) f.close();
  bool ok = isDir ? volFS().rmdir(path) : volFS().remove(path);
  if (ok) {
    if (isStoreVol()) libPathRemoved(path);
    server.send(200, "text/plain", "OK");
  } else server.send(500, "text/plain", isDir ? "Folder must be empty" : "Delete failed");
}

void handleMkdir() {
  if (!requireVol()) return;
  String path = safePath(server.arg("path"));
  if (volFS().mkdir(path)) server.send(200, "text/plain", "OK");
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

static bool copyFile(fs::FS& srcFs, const char* src, fs::FS& dstFs, const char* dst) {
  File in = srcFs.open(src, FILE_READ);
  if (!in || in.isDirectory()) return false;
  File out = dstFs.open(dst, FILE_WRITE);
  uint8_t* buf = (uint8_t*)malloc(4096);
  bool ok = out && buf;
  while (ok && in.available()) {
    size_t n = in.read(buf, 4096);
    if (n == 0) break;
    ok = out.write(buf, n) == n;
  }
  free(buf);
  in.close();
  if (out) out.close();
  if (!ok) dstFs.remove(dst);
  return ok;
}

// Keep / put back the stick's copy of the car's lock chime. Both need the
// car's drive mounted by the stick (Wi-Fi mode, or before USB mode starts).
void backupLockChime() {
  if (storeMounted && SD_MMC.exists("/LockChime.wav")) copyFile(SD_MMC, "/LockChime.wav", STORE, CHIME_BACKUP);
}
bool restoreLockChime() {
  return storeMounted && STORE.exists(CHIME_BACKUP) && copyFile(STORE, CHIME_BACKUP, SD_MMC, "/LockChime.wav");
}

// Copy a .wav to /LockChime.wav at the card root. The original is left untouched.
// The copy goes to a temp file first so a failed copy never destroys an existing chime.
// Returns "" on success, otherwise a message describing what went wrong.
String makeLockChime(fs::FS& srcFs, const String& src, bool fromCar) {
  String lower = src;
  lower.toLowerCase();
  if (!lower.endsWith(".wav")) return "Only .wav files can be used as a lock chime.";
  if (fromCar && lower == "/lockchime.wav") return "That file is already the lock chime.";
  File in = srcFs.open(src, FILE_READ);
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
  backupLockChime();                   // so the chime survives the car's drive being re-created
  return "";
}

// Copy a chime to the car's drive, showing each step on the screen, and then
// ask for a restart (done from loop(), after the web page has its answer). The
// car only reads the new sound when the stick restarts: taking the drive away
// and giving it back is not enough.
bool restartRequested = false;
String applyLockChime(fs::FS& srcFs, const String& src, bool fromCar) {
  const char* H = "Lock Chime";
  String name = src.substring(src.lastIndexOf('/') + 1);
  bool wasUsb = usbMode;
  if (wasUsb) {
    showBig(H, "Taking drive from car", "EJECT", name, C_ORANGE, 30000);
    setUsbMode(false);
  }
  showBig(H, "Copying the sound", "COPY", name, C_CYAN, 30000);
  String err = makeLockChime(srcFs, src, fromCar);
  if (err.length()) {
    if (wasUsb) setUsbMode(true);      // nothing changed: the car gets its drive back
    showToast("Chime failed", err, 4000, false);
    return err;
  }
  showBig(H, "New lock chime set", "DONE", name, C_GREEN, 30000);
  restartRequested = true;
  return "";
}

void handleLockChime() {
  if (!requireVol()) return;
  String err = applyLockChime(volFS(), safePath(server.arg("path")), !isStoreVol());
  if (err.length() == 0) { server.send(200, "text/plain", "OK"); return; }
  int code = 500;
  if (err == "File not found.") code = 404;
  else if (err.startsWith("Only") || err.startsWith("That")) code = 400;
  server.send(code, "text/plain", err);
}

// Stream a file to the browser for listening (the web page's Play button).
void handlePlay() {
  if (!requireVol()) return;
  String path = safePath(server.arg("path"));
  File f = volFS().open(path, FILE_READ);
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
  if (!requireVol()) return;
  String path = safePath(server.arg("path"));
  File f = volFS().open(path, FILE_READ);
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
  bool conn = (WiFi.status() == WL_CONNECTED);
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
  bool conn = (WiFi.status() == WL_CONNECTED);
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
  bool saveOnly = server.arg("hidden") == "1";   // the "Hidden / not listed" tick box: remember it, don't connect now
  bool force = server.arg("force") == "1";       // "try anyway" after the network was not found in a scan
  bool hidden = saveOnly || force;               // it is stored as a hidden network

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

  // "Hidden / not listed": just remember the network. It is joined later by the automatic
  // join (which tries hidden networks when nothing else matches), not now.
  if (saveOnly) {
    addOrUpdateNet(ssid, pass, true);
    staConnectSaved();                // look for it soon
    server.send(200, "text/plain", "Saved. The stick will join \"" + ssid + "\" when it is in range.");
    return;
  }

  // Make sure the network exists (skipped when the user said to try anyway).
  if (!force) {
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
  bool wasCurrent = WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid;
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
  bool on = server.arg("usb") == "1";
  setUsbMode(on);
  if (!on) giveStoreToHost();          // PC Mode: a computer on the USB port gets the library
  server.send(200, "text/plain", "OK");
}

void handleStoreTake() {               // edit the library from the web page again
  takeStoreBack();
  server.send(200, "text/plain", "OK");
}

// Restart button on the web page: the same clean restart as the BOOT button's long press (the drive is let go of
// properly, the car sees the stick unplugged and plugged in again). It always comes back in Car Mode.
void handleReboot() {
  server.send(200, "text/plain", "OK");
  restartRequested = true;             // the main loop does it after the answer has been sent
}

// ---------------- Random lock chime (BOOT button) ----------------
String lastChime;   // the file picked last time, so a press tries to choose a different one

// A file the car should be able to play: a real PCM WAV, 16-bit, mono or stereo.
static bool isGoodChimeWav(File& f) {
  WavInfo w;
  return readWavInfo(f, w) && w.fmt == 1 && w.bits == 16 && w.ch >= 1 && w.ch <= 2;
}

// The random pick: only sounds in a category marked "Random" (UnCategorized counts as one), and only files the car
// should be able to play (16-bit PCM WAV, under the size limit).
static void pickRandomFromLibrary(const String& skip, String& chosen, uint32_t& count) {
  libReconcile();
  uint32_t randMask = 0;
  for (int i = 1; i <= MAX_CATS; i++) if (libCats[i].used && libCats[i].rand) randMask |= (1UL << i);
  for (const LibSound& s : libSounds) {
    bool wanted = s.mask == 0 ? libCats[0].rand : (s.mask & randMask) != 0;
    if (!wanted || s.path == skip) continue;
    File f = STORE.open(s.path, FILE_READ);
    if (!f || f.isDirectory()) continue;
    if (f.size() > 0 && f.size() < cfgMaxChime && isGoodChimeWav(f)) {
      count++;
      if (esp_random() % count == 0) chosen = s.path;
    }
    f.close();
  }
}

// Picks a random sound from the Random categories and makes it the lock chime (the stick then restarts). Used by the
// stick's button and by the web page. Returns "" when it worked, otherwise what went wrong (also shown on the screen).
// *picked gets the display name of the chosen sound.
String randomLockChime(String* picked = nullptr) {
  String err;
  if (hostView == HOST_STORE) {
    err = "The computer is using the sound library";
    showToast("Lock Chime", err, 3000, false);
    return err;
  }
  if (!storeMounted) {
    err = "No sound library on the card";
    showToast("Lock Chime", err, 3000, false);
    return err;
  }
  libLoad();
  bool anyCategory = libCats[0].rand;
  for (int i = 1; i <= MAX_CATS; i++) if (libCats[i].used && libCats[i].rand) anyCategory = true;
  if (!anyCategory) {
    err = "No category is marked Random";
    showToast("Chime failed", err, 4000, false);
    return err;
  }
  showBig("Lock Chime", "Picking a sound", "PICK", "from the Random categories", C_CYAN, 30000);
  String chosen;
  uint32_t count = 0;
  pickRandomFromLibrary(lastChime, chosen, count);   // first try: anything except last time's pick
  if (count == 0) pickRandomFromLibrary("", chosen, count);   // only one candidate: allow a repeat
  if (count == 0) {
    err = "No suitable .wav files in the Random categories";
    showToast("Chime failed", err, 4000, false);
    return err;
  }
  err = applyLockChime(STORE, chosen, false);
  if (err.length() == 0) {
    lastChime = chosen;
    if (picked) {
      int k = libFind(chosen);
      *picked = k >= 0 ? libSounds[k].name : chosen.substring(chosen.lastIndexOf('/') + 1);
    }
  }
  return err;
}

// POST /random: the web page's "Random chime" button.
void handleRandomChime() {
  if (!requireStore()) return;
  String name;
  String err = randomLockChime(&name);
  if (err.length()) { server.send(409, "text/plain", err); return; }
  server.send(200, "text/plain", name);
}

// Restart, after letting go of the card and the USB drive cleanly. To the car this looks like unplugging the stick and
// plugging it in again.
void restartStick() {
  delay(700);                          // leave the last screen up long enough to read
  showBig("Restart", "Restarting the stick", "...", "Drive will reconnect", C_CYAN, 30000);
  if (uploadFile) uploadFile.close();
  msc.mediaPresent(false);
  delay(800);                          // let the host see the drive go
  STORE.end();
  xSemaphoreTake(sdLock, portMAX_DELAY);
  hostView = HOST_NONE;
  SD_MMC.end();
  ESP.restart();
}

// ---------------- Firmware updates ----------------
// The flash has two app slots (app0 / app1, see partitions.csv). An update is written into the slot that is NOT
// running and only then does the stick switch to it, so the running firmware is never touched. Nothing else in
// flash is written: the settings ("cfg"), the saved Wi-Fi networks (NVS) and the SD card all stay as they are.
//
// Two ways to get the new firmware onto the stick, both from the web page:
//   - the stick downloads it from GitHub itself (it must be joined to a home Wi-Fi that has internet), or
//   - the web page sends it: the browser downloads firmware-app.bin from GitHub (or you pick the file from a
//     GitHub release) and uploads it to /update/upload.
// Where the update is: the GitHub Pages site (UPDATE_BASE_URL). The build workflow publishes there
//   latest.json       {"version":"0.13","file":"firmware-app.bin","size":1234567,"md5":"..."}
//   firmware-app.bin  the app image only (not the bootloader/partition table)
#define UPDATE_CHECK_MS  (6UL * 3600UL * 1000UL)    // look for an update this often while on the internet
#define UPDATE_RETRY_MS  (15UL * 60UL * 1000UL)     // ... or this soon after a failed look
#define APP_MIN_BYTES    100000UL
#define APP_MAX_BYTES    0x640000UL                 // size of an app slot (partitions.csv)

// The certificate bundle that is built into the Arduino core (all the usual root CAs), used to check that we
// are really talking to github.io.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

// Compares dotted version numbers ("0.9" < "0.12"). Returns -1, 0 or 1.
static int cmpVersion(const String& a, const String& b) {
  size_t i = 0, j = 0;
  while (i < a.length() || j < b.length()) {
    long x = 0, y = 0;
    while (i < a.length() && isdigit((unsigned char)a[i])) x = x * 10 + (a[i++] - '0');
    while (j < b.length() && isdigit((unsigned char)b[j])) y = y * 10 + (b[j++] - '0');
    if (x != y) return x < y ? -1 : 1;
    while (i < a.length() && !isdigit((unsigned char)a[i])) i++;
    while (j < b.length() && !isdigit((unsigned char)b[j])) j++;
  }
  return 0;
}

// Reads one value of a flat JSON object ("key":"text" or "key":123). No escapes are expected.
static String jsonValue(const String& js, const char* key) {
  String k = String("\"") + key + "\"";
  int p = js.indexOf(k);
  if (p < 0) return "";
  p = js.indexOf(':', p + k.length());
  if (p < 0) return "";
  p++;
  while (p < (int)js.length() && isspace((unsigned char)js[p])) p++;
  if (p >= (int)js.length()) return "";
  if (js[p] == '"') {
    int q = js.indexOf('"', p + 1);
    return q < 0 ? String("") : js.substring(p + 1, q);
  }
  int q = p;
  while (q < (int)js.length() && js[q] != ',' && js[q] != '}' && !isspace((unsigned char)js[q])) q++;
  return js.substring(p, q);
}

static bool updHttpBegin(HTTPClient& http, NetworkClientSecure& tls, const String& url) {
  tls.setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  http.setUserAgent("TeslaWiFiUSBStick/" FW_VERSION);
  return http.begin(tls, url);
}

static bool updFail(const String& why) {
  updErr = why;
  updState = UPD_ERROR;
  updRunning = false;
  return false;
}

// Asks GitHub which version is the newest. Fills updLatest / updFile / updSize / updMd5 / updAvail.
static bool updCheck() {
  if (WiFi.status() != WL_CONNECTED) {
    return updFail("The stick is not on a Wi-Fi network with internet. Join your home Wi-Fi first.");
  }
  updState = UPD_CHECKING;
  NetworkClientSecure tls;
  HTTPClient http;
  if (!updHttpBegin(http, tls, UPDATE_BASE_URL "latest.json")) return updFail("Could not start the request");
  int code = http.GET();
  if (code != 200) {
    http.end();
    return updFail(code < 0 ? String("Could not reach GitHub") : "GitHub answered " + String(code));
  }
  String body = http.getString();
  http.end();
  if (body.length() > 2000) return updFail("Unexpected answer from GitHub");

  String v = jsonValue(body, "version"), file = jsonValue(body, "file"), md5 = jsonValue(body, "md5");
  uint32_t size = (uint32_t)jsonValue(body, "size").toInt();
  bool md5ok = md5.length() == 32;
  for (size_t i = 0; md5ok && i < md5.length(); i++) md5ok = isxdigit((unsigned char)md5[i]);
  // The file must sit next to latest.json (no other host, no folders up).
  bool fileOk = file.endsWith(".bin") && file.indexOf("..") < 0 && file.indexOf(':') < 0 && file.indexOf('/') < 0;
  if (v.length() == 0 || !md5ok || !fileOk || size < APP_MIN_BYTES || size > APP_MAX_BYTES) {
    return updFail("latest.json on GitHub is not in the expected form");
  }
  updLatest = v;
  updFile = file;
  updSize = size;
  updMd5 = md5;
  updAvail = cmpVersion(updLatest, FW_VERSION) > 0;
  updErr = "";
  updState = UPD_IDLE;
  return true;
}

// Is this the start of an ESP32-S3 app image (and not, say, a full image with a bootloader)?
static const char* appImageProblem(const uint8_t* d, size_t n) {
  if (n < 40) return "That file is too small to be firmware";
  if (d[0] != 0xE9) return "That is not an ESP32 firmware file";
  if (d[12] != 0x09 || d[13] != 0x00) return "That firmware is for a different chip (it must be for the ESP32-S3)";
  uint32_t magic = d[32] | (d[33] << 8) | (d[34] << 16) | ((uint32_t)d[35] << 24);
  if (magic != 0xABCD5432UL) {
    return "That is not the app-only firmware file. Use the file named ...-app.bin (not the one named ...-full.bin)";
  }
  return nullptr;
}

static void updShowProgress(int pct, bool force = false) {
  static int shown = -10;
  if (!force && pct < shown + 2 && pct != 100) return;
  shown = pct;
  showBig("Update", "Installing firmware", String(pct) + "%", "Do not unplug", C_ORANGE, 120000);
}

// Downloads the firmware named by the last check from GitHub, writes it to the other app slot and restarts.
static void updInstall() {
  if (updLatest.length() == 0 || updFile.length() == 0) { updFail("Check for an update first"); return; }
  if (WiFi.status() != WL_CONNECTED) { updFail("The stick is not on a Wi-Fi network with internet"); return; }
  updRunning = true;
  updState = UPD_DOWNLOADING;
  updPct = 0;
  updErr = "";
  updShowProgress(0, true);

  NetworkClientSecure tls;
  HTTPClient http;
  if (!updHttpBegin(http, tls, String(UPDATE_BASE_URL) + updFile)) { updFail("Could not start the download"); return; }
  int code = http.GET();
  if (code != 200) {
    http.end();
    updFail(code < 0 ? String("Could not reach GitHub") : "GitHub answered " + String(code));
    return;
  }
  int len = http.getSize();
  if (len <= 0 || (uint32_t)len != updSize) { http.end(); updFail("The download is not the size latest.json promised"); return; }
  if (!Update.begin(len, U_FLASH)) {
    http.end();
    updFail(String("Cannot start the update: ") + Update.errorString());
    return;
  }
  Update.setMD5(updMd5.c_str());     // checked before the stick switches to the new firmware

  NetworkClient* stream = http.getStreamPtr();
  static uint8_t buf[1460];
  int done = 0;
  uint32_t lastData = millis();
  while (done < len) {
    int avail = stream->available();
    if (avail > 0) {
      int n = stream->readBytes(buf, avail > (int)sizeof(buf) ? (int)sizeof(buf) : avail);
      if (n <= 0) continue;
      if (done == 0) {
        const char* bad = appImageProblem(buf, n);
        if (bad) { Update.abort(); http.end(); updFail(bad); return; }
      }
      if (Update.write(buf, n) != (size_t)n) {
        String e = Update.errorString();
        Update.abort();
        http.end();
        updFail("Writing the firmware failed: " + e);
        return;
      }
      done += n;
      lastData = millis();
      updPct = (int)((int64_t)done * 100 / len);
      updShowProgress(updPct);
    } else {
      if (!http.connected() && stream->available() == 0) break;
      if (millis() - lastData > 20000) break;
      delay(2);
    }
    server.handleClient();           // keep the web page answering (progress, busy messages)
  }
  http.end();
  if (done != len) { Update.abort(); updFail("The download stopped half-way. Nothing was changed; try again"); return; }
  if (!Update.end(true)) {
    String e = Update.errorString();
    updFail("The firmware did not pass the check (" + e + "). Nothing was changed");
    return;
  }

  updPct = 100;
  updState = UPD_DONE;
  updShowProgress(100, true);
  uint32_t until = millis() + 2500;                    // let the web page see "done" before the restart
  while ((int32_t)(millis() - until) < 0) { server.handleClient(); delay(5); }
  restartRequested = true;                             // the main loop restarts the stick into the new firmware
}

// Runs from loop(): the periodic look for updates, and the work the web page asked for.
void updTick() {
  static bool scheduled = false;
  static uint32_t nextCheck = 0;
  if (WiFi.status() != WL_CONNECTED) { scheduled = false; return; }   // no internet link: nothing to do
  if (!scheduled) { scheduled = true; nextCheck = millis() + 20000; }  // first look 20 s after joining
  if (updInstallRequested) {
    updInstallRequested = false;
    updInstall();
    return;
  }
  if (updCheckRequested || (int32_t)(millis() - nextCheck) >= 0) {
    updCheckRequested = false;
    bool ok = updCheck();
    nextCheck = millis() + (ok ? UPDATE_CHECK_MS : UPDATE_RETRY_MS);
  }
}

// ---- update web handlers ----
void handleUpdateInfo() {
  const char* st = updState == UPD_CHECKING ? "checking" : updState == UPD_DOWNLOADING ? "downloading"
                 : updState == UPD_DONE ? "done" : updState == UPD_ERROR ? "error" : "idle";
  String out = "{\"cur\":\"" FW_VERSION "\",\"latest\":\"" + jsonEscape(updLatest) + "\"";
  out += ",\"avail\":" + String(updAvail ? 1 : 0);
  out += ",\"net\":" + String(WiFi.status() == WL_CONNECTED ? 1 : 0);   // can the stick itself reach GitHub?
  out += ",\"state\":\"" + String(st) + "\",\"pct\":" + String((int)updPct);
  out += ",\"err\":\"" + jsonEscape(updErr) + "\"";
  out += ",\"host\":\"" + hostName + "\",\"base\":\"" UPDATE_BASE_URL "\"}";
  server.send(200, "application/json", out);
}

void handleUpdateCheck() {
  if (updRunning) { server.send(409, "text/plain", "An update is being installed"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(409, "text/plain", "The stick is not on a Wi-Fi network with internet"); return; }
  updCheckRequested = true;
  server.send(200, "text/plain", "OK");
}

void handleUpdateInstall() {
  if (updRunning) { server.send(409, "text/plain", "An update is already being installed"); return; }
  if (WiFi.status() != WL_CONNECTED) { server.send(409, "text/plain", "The stick is not on a Wi-Fi network with internet"); return; }
  if (!updAvail) { server.send(409, "text/plain", "No newer firmware is known. Check for updates first"); return; }
  updInstallRequested = true;
  updState = UPD_DOWNLOADING;       // so the page shows progress straight away
  updPct = 0;
  server.send(200, "text/plain", "OK");
}

// The web page (or you) sends the app image: POST /update/upload, multipart form with one file.
static bool upBad = false, upBusy = false;
static String upErr;
static size_t upGot = 0;
void handleUpdateUploadData() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    upBad = false; upBusy = false; upErr = ""; upGot = 0;
    if (updRunning) { upBad = true; upBusy = true; upErr = "An update is already being installed"; return; }
    updRunning = true;
    updState = UPD_DOWNLOADING;
    updPct = 0;
    showBig("Update", "Receiving firmware", "0%", "Do not unplug", C_ORANGE, 120000);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) { upBad = true; upErr = String("Cannot start the update: ") + Update.errorString(); }
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (upBad) return;
    if (upGot == 0) {
      const char* bad = appImageProblem(up.buf, up.currentSize);
      if (bad) { upBad = true; upErr = bad; Update.abort(); return; }
    }
    if (Update.write(up.buf, up.currentSize) != up.currentSize) {
      upBad = true; upErr = "Writing the firmware failed: " + String(Update.errorString()); Update.abort(); return;
    }
    upGot += up.currentSize;
    int total = server.clientContentLength();
    if (total > 0) { updPct = (int)((int64_t)upGot * 100 / total); if (updPct > 99) updPct = 99; updShowProgress(updPct); }
  } else if (up.status == UPLOAD_FILE_END) {
    if (upBad) return;
    if (upGot < APP_MIN_BYTES) { upBad = true; upErr = "That file is too small to be the firmware"; Update.abort(); return; }
    if (!Update.end(true)) { upBad = true; upErr = "The firmware did not pass the check (" + String(Update.errorString()) + "). Nothing was changed"; }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (!upBad) Update.abort();
    upBad = true;
    upErr = "The upload was interrupted. Nothing was changed";
  }
}

void handleUpdateUploadDone() {
  if (upBusy) { server.send(409, "text/plain", upErr); return; }     // another update is running: leave its state alone
  if (upBad || upGot == 0) {
    if (!upBad) upErr = "No file was received";
    updFail(upErr);
    lastScreenKey = "";            // redraw the normal screen
    toastUntil = 0;
    server.send(400, "text/plain", upErr);
    return;
  }
  updPct = 100;
  updState = UPD_DONE;
  updShowProgress(100, true);
  server.send(200, "text/plain", "OK");
  restartRequested = true;         // the main loop restarts the stick into the new firmware
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

// ---------------- Network name ----------------
// "TeslaWiFiUSBStick-D4F1A2": the fixed name plus the last 3 bytes of the Wi-Fi MAC address, so several sticks on
// one network have different names. It is the DHCP host name (shown in your router's list of devices) and the
// mDNS name, so http://TeslaWiFiUSBStick-D4F1A2.local opens the web page from your home network.
void initHostName() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char suffix[8];
  snprintf(suffix, sizeof(suffix), "-%02X%02X%02X", mac[3], mac[4], mac[5]);
  hostName = String(HOST_PREFIX) + suffix;
}

// Stops web requests that change the card or Wi-Fi while a firmware update is being installed.
#define BUSY_GUARD(fn) ([]() { if (updRunning) { server.send(503, "text/plain", "A firmware update is being installed"); return; } fn(); })

// ---------------- Setup / loop ----------------
void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  sdLock = xSemaphoreCreateMutex();
  loadConfig();        // settings written by the web installer (if any)
  displayInit();
  bool repaired = false;

  bool mounted = mountSD();
  if (!mounted && rawCardOpen()) {
    // There is a card, but no file system the stick can mount.
    readLayout(rawCardSectors());
    if (layout.split) {
      // A set-up card whose car drive won't mount. Try once more, then re-create
      // just that drive; the sound library is left alone.
      rawCardClose();
      delay(300);
      mounted = mountSD();
      if (!mounted && rawCardOpen()) {
        showToast("Repairing", "Car drive damaged: re-creating it. Do not unplug.", 600000, false);
        formatting = true;
        const char* err = formatBoombox(esp_random());
        formatting = false;
        rawCardClose();
        repaired = (err[0] == 0);
        mounted = repaired && mountSD();
      }
      if (!mounted) cardProblem = "Car drive unreadable";
    } else if (!layout.readError) {
      firstRun = true;             // new, exFAT or otherwise unusable: set it up
    } else {
      rawCardClose();
    }
  }
  if (mounted) {
    sectorCount = cardSectors();
    cardTotal = SD_MMC.totalBytes();
    readLayout(sectorCount);       // the car only sees the Boombox part
    if (layout.split) {
      STORE.begin();               // the sound library (CHIME_STORE)
      if (repaired) {
        bool chime = restoreLockChime();
        showToast("Drive repaired", chime ? "Lock chime put back" : "No lock chime to put back", 4000, true);
      } else if (storeMounted && !STORE.exists(CHIME_BACKUP)) {
        backupLockChime();         // a chime from before the stick kept copies
      }
    } else if (!layout.readError) {
      firstRun = true;             // an ordinary one-partition card
    }
  }
  // No card at all: keep the web UI up so you can see something is wrong.
  if (layout.readError) cardProblem = "SD card read error";
  if (!firstRun && !(layout.split && mounted)) {      // nothing usable: the car gets no drive
    sectorCount = 0;
    cardTotal = 0;
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
  initHostName();
  WiFi.setHostname(hostName.c_str());   // must be set before the interface starts
  WiFi.mode(WIFI_AP_STA);          // own access point + optional router link
  WiFi.setAutoReconnect(true);
  WiFi.softAP(cfgSsid.c_str(), cfgPass.c_str(), cfgChannel, cfgHidden);
  WiFi.softAPsetHostname(hostName.c_str());
  loadWifiPrefs();
  if (MDNS.begin(hostName.c_str())) MDNS.addService("http", "tcp", 80);   // http://<name>.local

  if (firstRun) {
    // Only the progress page: no file or mode requests until the card is set up.
    server.on("/", HTTP_GET, handleFirstRunRoot);
    server.on("/api/status", HTTP_GET, handleFirstRunStatus);
    server.begin();
    firstRunAt = millis();
    return;
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/card/rebuild", HTTP_POST, BUSY_GUARD(handleCardRebuild));
  server.on("/api/list", HTTP_GET, handleList);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/upload", HTTP_POST, BUSY_GUARD(handleUploadDone), handleUploadData);
  server.on("/rename", HTTP_POST, BUSY_GUARD(handleRename));
  server.on("/delete", HTTP_POST, BUSY_GUARD(handleDelete));
  server.on("/mkdir", HTTP_POST, BUSY_GUARD(handleMkdir));
  server.on("/lockchime", HTTP_POST, BUSY_GUARD(handleLockChime));
  server.on("/mode", HTTP_POST, BUSY_GUARD(handleMode));
  server.on("/store/take", HTTP_POST, BUSY_GUARD(handleStoreTake));
  server.on("/reboot", HTTP_POST, BUSY_GUARD(handleReboot));
  server.on("/random", HTTP_POST, BUSY_GUARD(handleRandomChime));
  server.on("/wifi/scan", HTTP_POST, BUSY_GUARD(handleWifiScan));
  server.on("/api/wifi/networks", HTTP_GET, handleWifiNetworks);
  server.on("/api/wifi/status", HTTP_GET, handleWifiStatus);
  server.on("/wifi/connect", HTTP_POST, BUSY_GUARD(handleWifiConnect));
  server.on("/api/wifi/saved", HTTP_GET, handleWifiSaved);
  server.on("/wifi/delete", HTTP_POST, BUSY_GUARD(handleWifiDelete));
  server.on("/play", HTTP_GET, handlePlay);
  server.on("/api/wavinfo", HTTP_GET, handleWavInfo);
  server.on("/api/lib", HTTP_GET, handleLibList);
  server.on("/lib/cat/add", HTTP_POST, BUSY_GUARD(handleLibCatAdd));
  server.on("/lib/cat/rename", HTTP_POST, BUSY_GUARD(handleLibCatRename));
  server.on("/lib/cat/delete", HTTP_POST, BUSY_GUARD(handleLibCatDelete));
  server.on("/lib/cat/rand", HTTP_POST, BUSY_GUARD(handleLibCatRandom));
  server.on("/lib/sound/set", HTTP_POST, BUSY_GUARD(handleLibSoundSet));
  server.on("/api/update", HTTP_GET, handleUpdateInfo);
  server.on("/update/check", HTTP_POST, handleUpdateCheck);
  server.on("/update/install", HTTP_POST, handleUpdateInstall);
  server.on("/update/upload", HTTP_POST, handleUpdateUploadDone, handleUpdateUploadData);
  server.begin();

  if (sectorCount > 0) setUsbMode(true);   // the car gets its drive straight away
  else staConnectSaved();          // no usable card: join the saved router so the page is easy to reach
}

void loop() {
  server.handleClient();

  if (firstRun) {                  // the card is being set up: nothing else runs
    firstRunTick();
    displayUpdate();
    return;
  }

  if (rebuildRequested) doCardRebuild();
  if (restartRequested) restartStick();   // a new lock chime was set (never returns)

  // The host ejected its drive: the stick takes it back (the car's drive -> PC Mode)
  if (ejectRequested) {
    ejectRequested = false;
    if (hostView == HOST_STORE) takeStoreBack();
    else if (usbMode) setUsbMode(false);
  }

  freeScanTick();      // library with a computer: count free clusters in small slices
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
      staConnectSaved();                         // go back to the remembered networks
    }
  }
  staAutoJoinTick();   // look for and join a remembered network
  updTick();           // look for a newer firmware on GitHub now and then; install it when asked

  // BOOT button: short press = random lock chime (then the stick restarts),
  // held for more than 5 seconds = restart only. Both act when the button is let
  // go, so it is never down while the chip resets (held down at reset, it would
  // start the flashing mode instead).
  static uint32_t downAt = 0;
  static int hint = 0;
  bool down = (digitalRead(BUTTON_PIN) == LOW);
  if (down) {
    if (downAt == 0) { downAt = millis() | 1; hint = 0; }
    uint32_t held = millis() - downAt;
    if (held >= BUTTON_LONG_MS && hint < 2) {
      hint = 2;
      showBig("Button", "Restart only", "LET GO", "to restart the stick", C_ORANGE, 60000);
    } else if (held >= 1000 && hint < 1) {
      hint = 1;
      showBig("Button", "Let go: new chime", "HOLD", "5 s: restart only", C_CYAN, 60000);
    }
  } else if (downAt != 0) {
    uint32_t held = millis() - downAt;
    downAt = 0;
    if (held >= BUTTON_LONG_MS) restartStick();
    else if (held >= 50) {                           // 50 ms: ignore contact bounce
      if (sectorCount > 0) randomLockChime();        // restarts the stick when it worked
      else showToast("Lock Chime", "No SD card", 3000, false);
    }
  }
}
