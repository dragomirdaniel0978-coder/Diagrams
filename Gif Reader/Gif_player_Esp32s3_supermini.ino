// ============================================================
//  GIF PLAYER — ESP32-S3 SuperMini + ST7735 128×128 + SD Card
//  Plays every .gif on the SD root, loops forever.
//  Tested with Arduino IDE 2.x + ESP32 Arduino Core 3.x
// ============================================================
//
//  ── WIRING ──────────────────────────────────────────────────
//  ESP32-S3 SuperMini   →   Device
//  GPIO 36  (SPI_MOSI)  →   TFT MOSI  +  SD MOSI
//  GPIO 35  (SPI_CLK)   →   TFT SCK   +  SD SCK
//  GPIO 37  (SPI_MISO)  →   SD MISO   (TFT MISO unused)
//  GPIO  4  (TFT_CS)    →   TFT CS
//  GPIO  5  (TFT_DC)    →   TFT DC/RS
//  GPIO  6  (TFT_RST)   →   TFT RST
//  GPIO 34  (SD_CS)     →   SD CS
//  3V3                  →   TFT VCC + SD VCC
//  GND                  →   TFT GND + SD GND
//
//  Both devices share the same SPI bus (HSPI), CS pins separate.
//
//  ── LIBRARY REQUIREMENTS (install via Library Manager) ──────
//  • Adafruit GFX Library          (Adafruit)
//  • Adafruit ST7735 and ST7789    (Adafruit)
//  • AnimatedGIF                   (Larry Bank / bitbank2)
//  SD and SPI are built into ESP32 Arduino core.
//
//  ── BOARD SETTINGS (Tools menu) ─────────────────────────────
//  Board       : ESP32S3 Dev Module
//  Flash Size  : 4MB (or match your board)
//  PSRAM       : Disabled  (we don't need it)
//  USB Mode    : Hardware CDC and JTAG
//  Upload Speed: 921600
//  Port        : your COMx / /dev/ttyUSB0
//
//  ── SD CARD FORMAT ──────────────────────────────────────────
//  FAT32, ≤ 32 GB recommended.
//  Place .gif files in the ROOT directory.
//  Filenames: 8.3 format  (ANIM.GIF)  OR long names — both work.
//
// ============================================================

#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <AnimatedGIF.h>

// ── Pin definitions ─────────────────────────────────────────
#define TFT_CS    4
#define TFT_DC    5
#define TFT_RST   6
#define SD_CS    34
#define SPI_MOSI 36
#define SPI_MISO 37
#define SPI_CLK  35

#define SCREEN_W 128
#define SCREEN_H 128

// ── Global objects ───────────────────────────────────────────
SPIClass         hspi(HSPI);
Adafruit_ST7735  tft  = Adafruit_ST7735(&hspi, TFT_CS, TFT_DC, TFT_RST);
AnimatedGIF      gif;

// ── GIF file list ────────────────────────────────────────────
#define MAX_GIFS 64
char    gifPaths[MAX_GIFS][64];
uint8_t gifCount  = 0;
uint8_t gifIndex  = 0;

// ── Active SD file handle shared with callbacks ──────────────
File gifFile;

// ============================================================
//  AnimatedGIF I/O callbacks  (file-backed)
// ============================================================

void * gifOpen(const char *filename, int32_t *pSize) {
  gifFile = SD.open(filename, FILE_READ);
  if (!gifFile) {
    Serial.printf("[GIF] Cannot open: %s\n", filename);
    return nullptr;
  }
  *pSize = gifFile.size();
  return (void*)&gifFile;
}

void gifClose(void *pHandle) {
  if (gifFile) gifFile.close();
}

int32_t gifRead(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen) {
  int32_t toRead = min(iLen, (int32_t)(pFile->iSize - pFile->iPos));
  if (toRead <= 0) return 0;
  int32_t n = gifFile.read(pBuf, toRead);
  pFile->iPos += n;
  return n;
}

int32_t gifSeek(GIFFILE *pFile, int32_t iPosition) {
  gifFile.seek(iPosition);
  pFile->iPos = iPosition;
  return iPosition;
}

// ============================================================
//  Draw callback — renders one decoded line-row to TFT
// ============================================================

void gifDraw(GIFDRAW *pDraw) {
  // pDraw delivers one horizontal stripe (y = pDraw->y, height 1)
  // in native GIF palette → we map to RGB565 and push to TFT.

  uint8_t  *src    = pDraw->pPixels;
  uint16_t *pal    = pDraw->pPalette;          // already RGB565
  int       x      = pDraw->iX;
  int       y      = pDraw->iY + pDraw->y;
  int       w      = pDraw->iWidth;

  // Clip
  if (y >= SCREEN_H || x >= SCREEN_W) return;
  if (x + w > SCREEN_W) w = SCREEN_W - x;

  // Build a row of RGB565 values
  static uint16_t lineBuf[SCREEN_W];
  bool hasTransparent = (pDraw->ucHasTransparency != 0);
  uint8_t transpIdx   = pDraw->ucTransparent;

  if (hasTransparent) {
    // Read back not possible easily; skip transparent pixels
    // by drawing runs of opaque pixels only
    int start = 0;
    while (start < w) {
      // skip transparent
      while (start < w && src[start] == transpIdx) start++;
      if (start >= w) break;
      // collect opaque run
      int run = start;
      while (run < w && src[run] != transpIdx) {
        lineBuf[run - start] = pal[src[run]];
        run++;
      }
      int runLen = run - start;
      tft.startWrite();
      tft.setAddrWindow(x + start, y, runLen, 1);
      tft.writePixels(lineBuf, runLen);
      tft.endWrite();
      start = run;
    }
  } else {
    for (int i = 0; i < w; i++) lineBuf[i] = pal[src[i]];
    tft.startWrite();
    tft.setAddrWindow(x, y, w, 1);
    tft.writePixels(lineBuf, w);
    tft.endWrite();
  }
}

// ============================================================
//  SD helpers — collect all .gif files from root
// ============================================================

void collectGIFs() {
  gifCount = 0;
  File root = SD.open("/");
  if (!root) {
    Serial.println("[SD] Cannot open root!");
    return;
  }

  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) { entry.close(); continue; }

    const char *name = entry.name();
    size_t len = strlen(name);

    // Case-insensitive .gif check
    if (len > 4) {
      const char *ext = name + len - 4;
      if ((ext[0]=='.' ) &&
          (ext[1]=='g' || ext[1]=='G') &&
          (ext[2]=='i' || ext[2]=='I') &&
          (ext[3]=='f' || ext[3]=='F')) {

        if (gifCount < MAX_GIFS) {
          // Build full path — SD library on ESP32 returns name without leading /
          snprintf(gifPaths[gifCount], sizeof(gifPaths[0]), "/%s", name);
          Serial.printf("[SD] Found GIF %d: %s\n", gifCount, gifPaths[gifCount]);
          gifCount++;
        }
      }
    }
    entry.close();
  }
  root.close();
  Serial.printf("[SD] Total GIFs found: %d\n", gifCount);
}

// ============================================================
//  Play one GIF file (all frames, respecting frame delay)
// ============================================================

void playGIF(const char *path) {
  Serial.printf("[GIF] Playing: %s\n", path);

  if (!gif.open(path, gifOpen, gifClose, gifRead, gifSeek, gifDraw)) {
    Serial.printf("[GIF] open() failed for %s\n", path);
    return;
  }

  Serial.printf("[GIF] %d x %d\n",
                gif.getCanvasWidth(), gif.getCanvasHeight());

  int frameResult;
  while (true) {
    frameResult = gif.playFrame(true, nullptr);
    // playFrame returns:
    //   1  → more frames remain
    //   0  → last frame rendered (loop done)
    //  -1  → error
    if (frameResult <= 0) break;
    yield(); // keep RTOS happy
  }

  gif.close();

  if (frameResult < 0) {
    Serial.printf("[GIF] playFrame error on %s\n", path);
  }
}

// ============================================================
//  Error screen — shown when something critical fails
// ============================================================

void showError(const char *msg) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_RED);
  tft.setTextSize(1);
  tft.setCursor(2, 10);
  tft.println("!! ERROR !!");
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(2, 26);
  tft.println(msg);
}

// ============================================================
//  setup()
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== GIF Player boot ===");

  // ── Init shared SPI bus ──────────────────────────────────
  hspi.begin(SPI_CLK, SPI_MISO, SPI_MOSI, -1);  // SCK, MISO, MOSI, no default SS

  // ── Init TFT ─────────────────────────────────────────────
  // initR variants: INITR_BLACKTAB, INITR_GREENTAB, INITR_REDTAB
  // Most 128x128 modules use BLACKTAB or GREENTAB — try BLACKTAB first.
  tft.initR(INITR_BLACKTAB);
  tft.setRotation(0);
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(1);
  tft.setCursor(10, 55);
  tft.println("GIF Player");
  tft.setCursor(10, 68);
  tft.setTextColor(ST77XX_WHITE);
  tft.println("Mounting SD...");
  Serial.println("[TFT] Initialized");

  // ── Init SD ──────────────────────────────────────────────
  if (!SD.begin(SD_CS, hspi)) {
    Serial.println("[SD] Mount FAILED");
    showError("SD mount failed.\nCheck card & wiring.");
    while (true) delay(1000); // halt
  }
  Serial.println("[SD] Mounted OK");

  uint8_t cardType = SD.cardType();
  Serial.printf("[SD] Card type: %s\n",
    cardType == CARD_MMC  ? "MMC"  :
    cardType == CARD_SD   ? "SD"   :
    cardType == CARD_SDHC ? "SDHC" : "UNKNOWN");
  Serial.printf("[SD] Card size: %llu MB\n", SD.cardSize() / (1024*1024));

  // ── Scan for GIF files ───────────────────────────────────
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(10, 55);
  tft.println("Scanning SD...");
  collectGIFs();

  if (gifCount == 0) {
    Serial.println("[GIF] No GIF files found on SD root!");
    showError("No .gif files\nfound in SD root!");
    while (true) delay(1000);
  }

  // ── Init AnimatedGIF library ─────────────────────────────
  gif.begin(LITTLE_ENDIAN_PIXELS);

  Serial.printf("[READY] %d GIF(s) queued. Starting playback.\n", gifCount);
}

// ============================================================
//  loop() — cycle through all GIFs, forever
// ============================================================

void loop() {
  playGIF(gifPaths[gifIndex]);
  gifIndex = (gifIndex + 1) % gifCount;
}

// ============================================================
//  END OF FILE
// ============================================================
