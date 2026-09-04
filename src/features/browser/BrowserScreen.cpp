#include "BrowserScreen.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <ctype.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>

// JPEGDEC's filesystem overload refers to File without its fs namespace.
using File = fs::File;
#include <JPEGDEC.h>
#include <PNGdec.h>
#include "../../../third_party/AnimatedGIF/AnimatedGIF.h"

#include "../../app/Navigator.h"
#include "BrowserText.h"
#include "../../services/storage/SdCardController.h"
#include "../../core/SpiBusLock.h"
#include "../../ui/UiKit.h"
#include "../../services/network/WiFiController.h"
#include "../../ui/icons.h"

namespace {
const int16_t HeaderHeight = UiKit::AppBarHeight;
const int16_t ControlsHeight = 42;
const int16_t TextRowHeight = 22;
const int16_t FormControlHeight = 42;
const int16_t FormTextareaHeight = 58;
const int16_t DocumentPadding = 7;
const int16_t DocumentSidePadding = 8;
const uint8_t StartListVisibleRows = 8;
const uint8_t ThumbnailMaxWidth = 200;
const uint8_t ThumbnailMaxHeight = 200;
constexpr uint32_t MaxEmbeddedImageBytes = BrowserLimits::MaxEmbeddedImageBytes;
const int16_t RecentListTop = HeaderHeight + 24;
const uint16_t FooterButtonColor = 0x2965;
const uint16_t DefaultTextColor = TFT_BLACK;
const uint16_t ImagePlaceholderColor = TFT_DARKGREY;
const uint16_t LinkImageColor = TFT_BLUE;
const char *ImagePlaceholderLabel = "image";
const char *DownloadDirectory = "/Downloads";
const char *SessionCacheDirectory = "/.system/browser_session";
// Some file hosts route unknown clients to an informational page instead of
// returning the requested media. The menu can temporarily present this
// conventional mobile identity so servers use their normal mobile response.
const char *EspUserAgent = "ESP-Media-Browser/1.0";
const char *MobileUserAgent =
  "Mozilla/5.0 (Linux; Android 10; K) AppleWebKit/537.36 "
  "(KHTML, like Gecko) Chrome/120.0.0.0 Mobile Safari/537.36";

void browserLog(const __FlashStringHelper *message) {
  Serial.print(F("[Browser] "));
  Serial.println(message);
}

void browserLog(const String &message) {
  Serial.print(F("[Browser] "));
  Serial.println(message);
}

// Thumbnail buffers cross from the image worker to the UI task.  Verify that
// a pointer still belongs to a heap before releasing it: an invalid value must
// not turn a cancelled page load into an ESP-IDF heap assertion and reboot.
void freeThumbnailPixels(uint16_t *pixels, const __FlashStringHelper *owner) {
  if (!pixels) return;
  if (!heap_caps_check_integrity_addr(reinterpret_cast<intptr_t>(pixels), false)) {
    browserLog(String(F("thumbnail buffer was not freed: invalid pointer in ")) + owner);
    return;
  }
  free(pixels);
}

// The TFT, SD card, and image decoders share the SPI bus.  Image decoding
// runs on the other core, so every Browser display transaction must wait for
// the same lock used by SD operations before touching the TFT.
class BrowserDisplayLock {
public:
  BrowserDisplayLock() : _locked(SpiBusLock::lock(pdMS_TO_TICKS(100))) {}
  ~BrowserDisplayLock() { if (_locked) SpiBusLock::unlock(); }
  bool locked() const { return _locked; }

private:
  bool _locked;
};

String trimCopy(const String &value) {
  String result = value;
  result.trim();
  return result;
}

uint8_t utf8CharacterBytes(const String &value, uint16_t offset) {
  if (offset >= value.length()) return 0;
  const uint8_t first = (uint8_t)value[offset];
  uint8_t length = first < 0x80 ? 1 : (first >= 0xc2 && first <= 0xdf ? 2 :
                   (first >= 0xe0 && first <= 0xef ? 3 : (first >= 0xf0 && first <= 0xf4 ? 4 : 1)));
  if (offset + length > value.length()) return 1;
  for (uint8_t i = 1; i < length; ++i) if (((uint8_t)value[offset + i] & 0xc0) != 0x80) return 1;
  return length;
}

String clientState(WiFiClient &client) {
  return String(F("connected=")) + (client.connected() ? F("yes") : F("no")) +
         F(", available=") + String(client.available());
}

bool startsWithIgnoreCase(const String &value, const char *prefix) {
  size_t length = strlen(prefix);
  return value.length() >= length && value.substring(0, length).equalsIgnoreCase(prefix);
}

void drawCheckbox(TFT_eSPI &tft, int16_t x, int16_t y, bool checked, uint16_t color) {
  tft.drawRect(x, y, 16, 16, color);
  if (!checked) return;
  tft.drawLine(x + 3, y + 8, x + 7, y + 12, color);
  tft.drawLine(x + 7, y + 12, x + 13, y + 4, color);
  tft.drawLine(x + 3, y + 9, x + 7, y + 13, color);
  tft.drawLine(x + 7, y + 13, x + 14, y + 4, color);
}

uint64_t parseHttpDate(const String &source) {
  String value = source;
  int comma = value.indexOf(',');
  if (comma >= 0) value = value.substring(comma + 1);
  value.trim();
  value.replace("-", " ");
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  char month[4] = {};
  if (sscanf(value.c_str(), "%d %3s %d %d:%d:%d", &day, month, &year, &hour, &minute, &second) != 6 ||
      year < 1970 || day < 1 || hour > 23 || minute > 59 || second > 59) return 0;
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  String monthText = month;
  monthText.toLowerCase();
  int monthIndex = -1;
  for (int i = 0; i < 12; i++) {
    String candidate = String(months).substring(i * 3, i * 3 + 3);
    candidate.toLowerCase();
    if (candidate == monthText) {
      monthIndex = i;
      break;
    }
  }
  if (monthIndex < 0) return 0;
  const uint8_t monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto leap = [](int valueYear) { return valueYear % 4 == 0 && (valueYear % 100 != 0 || valueYear % 400 == 0); };
  int maxDay = monthDays[monthIndex] + (monthIndex == 1 && leap(year) ? 1 : 0);
  if (day > maxDay) return 0;
  uint64_t days = 0;
  for (int valueYear = 1970; valueYear < year; valueYear++) days += leap(valueYear) ? 366 : 365;
  for (int i = 0; i < monthIndex; i++) days += monthDays[i] + (i == 1 && leap(year) ? 1 : 0);
  days += day - 1;
  return days * 86400ULL + hour * 3600UL + minute * 60UL + second;
}

// Decoding happens only while a document is loading, so a single callback
// owner is sufficient. The decoder writes a tiny RGB565 surface rather than
// allocating a full-size source canvas.
class ThumbnailDecoder {
public:
  ~ThumbnailDecoder() {
    discard();
  }

  bool decode(const String &path, FileFormats::Format format, uint16_t *&pixels,
              uint8_t &width, uint8_t &height, uint16_t backgroundColor) {
    pixels = nullptr;
    width = 0;
    height = 0;
    _backgroundColor = backgroundColor;
    if (format == FileFormats::Format::Jpeg) return decodeJpeg(path, pixels, width, height);
    if (format == FileFormats::Format::Png) return decodePng(path, pixels, width, height);
    if (format == FileFormats::Format::Gif) return decodeGif(path, pixels, width, height);
    return false;
  }

private:
  static ThumbnailDecoder *_owner;
  JPEGDEC _jpeg;
  PNG _png;
  AnimatedGIF *_gif = nullptr;
  File _file;
  uint16_t *_pixels = nullptr;
  uint8_t _width = 0;
  uint8_t _height = 0;
  uint16_t _sourceWidth = 0;
  uint16_t _sourceHeight = 0;
  uint16_t _decodedWidth = 0;
  uint16_t _decodedHeight = 0;
  uint16_t _backgroundColor = TFT_WHITE;

  bool prepare(uint16_t sourceWidth, uint16_t sourceHeight, uint16_t decodedWidth,
               uint16_t decodedHeight, uint16_t *&pixels, uint8_t &width, uint8_t &height) {
    if (!sourceWidth || !sourceHeight || !decodedWidth || !decodedHeight) return false;
    uint32_t scaledWidth = sourceWidth;
    uint32_t scaledHeight = sourceHeight;
    if (scaledWidth > ThumbnailMaxWidth) {
      scaledHeight = (scaledHeight * ThumbnailMaxWidth) / scaledWidth;
      scaledWidth = ThumbnailMaxWidth;
    }
    if (scaledHeight > ThumbnailMaxHeight) {
      scaledWidth = (scaledWidth * ThumbnailMaxHeight) / scaledHeight;
      scaledHeight = ThumbnailMaxHeight;
    }
    if (scaledWidth == 0) scaledWidth = 1;
    if (scaledHeight == 0) scaledHeight = 1;
    const size_t byteCount = (size_t)scaledWidth * scaledHeight * sizeof(uint16_t);
    // Page previews may accumulate while the worker continues through an
    // image-heavy page. Keep their backing storage out of scarce internal
    // RAM on PSRAM-equipped ESP32-S3 boards.
    _pixels = static_cast<uint16_t *>(psramFound() ? ps_malloc(byteCount) : malloc(byteCount));
    if (!_pixels) return false;
    for (uint32_t index = 0; index < (uint32_t)scaledWidth * scaledHeight; index++) {
      _pixels[index] = _backgroundColor;
    }
    _width = (uint8_t)scaledWidth;
    _height = (uint8_t)scaledHeight;
    _sourceWidth = sourceWidth;
    _sourceHeight = sourceHeight;
    _decodedWidth = decodedWidth;
    _decodedHeight = decodedHeight;
    pixels = _pixels;
    width = _width;
    height = _height;
    return true;
  }

  void discard() {
    if (_file) _file.close();
    if (_gif) {
      _gif->close();
      delete _gif;
      _gif = nullptr;
    }
    freeThumbnailPixels(_pixels, F("thumbnail decoder"));
    _pixels = nullptr;
    _width = _height = 0;
  }

  bool decodeJpeg(const String &path, uint16_t *&pixels, uint8_t &width, uint8_t &height) {
    _file = SD.open(path.c_str(), FILE_READ);
    if (!_file) return false;
    _owner = this;
    if (_jpeg.open(_file, jpegDraw) != 1 || _jpeg.getJPEGType() != JPEG_MODE_BASELINE) {
      _jpeg.close();
      _owner = nullptr;
      discard();
      return false;
    }
    const uint16_t sourceWidth = _jpeg.getWidth();
    const uint16_t sourceHeight = _jpeg.getHeight();
    uint8_t scale = 1;
    while (scale < 8 && (sourceWidth / scale > ThumbnailMaxWidth * 2 ||
                         sourceHeight / scale > ThumbnailMaxHeight * 2)) scale *= 2;
    const uint16_t decodedWidth = (sourceWidth + scale - 1) / scale;
    const uint16_t decodedHeight = (sourceHeight + scale - 1) / scale;
    if (!prepare(sourceWidth, sourceHeight, decodedWidth, decodedHeight, pixels, width, height)) {
      _jpeg.close();
      _owner = nullptr;
      discard();
      return false;
    }
    int options = scale == 8 ? JPEG_SCALE_EIGHTH : scale == 4 ? JPEG_SCALE_QUARTER :
                  scale == 2 ? JPEG_SCALE_HALF : 0;
    const bool ok = _jpeg.decode(0, 0, options) == 1;
    _jpeg.close();
    _owner = nullptr;
    if (ok) {
      if (_file) _file.close();
      _pixels = nullptr;  // caller owns the completed thumbnail.
      return true;
    }
    discard();
    pixels = nullptr;
    width = height = 0;
    return false;
  }

  bool decodePng(const String &path, uint16_t *&pixels, uint8_t &width, uint8_t &height) {
    _owner = this;
    if (_png.open(path.c_str(), pngOpen, pngClose, pngRead, pngSeek, pngDraw) != PNG_SUCCESS ||
        _png.isInterlaced()) {
      _png.close();
      _owner = nullptr;
      discard();
      return false;
    }
    if (!prepare((uint16_t)_png.getWidth(), (uint16_t)_png.getHeight(),
                 (uint16_t)_png.getWidth(), (uint16_t)_png.getHeight(), pixels, width, height)) {
      _png.close();
      _owner = nullptr;
      discard();
      return false;
    }
    const bool ok = _png.decode(this, PNG_CHECK_CRC) == PNG_SUCCESS;
    _png.close();
    _owner = nullptr;
    if (ok) {
      if (_file) _file.close();
      _pixels = nullptr;
      return true;
    }
    discard();
    pixels = nullptr;
    width = height = 0;
    return false;
  }

  bool decodeGif(const String &path, uint16_t *&pixels, uint8_t &width, uint8_t &height) {
    _gif = new AnimatedGIF();
    if (!_gif) return false;
    _gif->begin(GIF_PALETTE_RGB565_LE);
    _owner = this;
    if (!_gif->open(path.c_str(), gifOpen, gifClose, gifRead, gifSeek, gifDraw)) {
      _owner = nullptr;
      discard();
      return false;
    }
    if (!prepare(_gif->getCanvasWidth(), _gif->getCanvasHeight(),
                 _gif->getCanvasWidth(), _gif->getCanvasHeight(), pixels, width, height)) {
      _owner = nullptr;
      discard();
      return false;
    }
    int delay = 0;
    const int result = _gif->playFrame(false, &delay, nullptr);
    const bool ok = result >= 0 && _gif->getLastError() == GIF_SUCCESS;
    _owner = nullptr;
    if (ok) {
      _gif->close();
      delete _gif;
      _gif = nullptr;
      if (_file) _file.close();
      _pixels = nullptr;
      return true;
    }
    discard();
    pixels = nullptr;
    width = height = 0;
    return false;
  }

  void store(uint16_t sourceX, uint16_t sourceY, uint16_t color, bool decodedCoordinates) {
    if (!_pixels) return;
    const uint16_t sourceWidth = decodedCoordinates ? _decodedWidth : _sourceWidth;
    const uint16_t sourceHeight = decodedCoordinates ? _decodedHeight : _sourceHeight;
    if (!sourceWidth || !sourceHeight) return;
    const uint8_t x = (uint32_t)sourceX * _width / sourceWidth;
    const uint8_t y = (uint32_t)sourceY * _height / sourceHeight;
    if (x < _width && y < _height) _pixels[(uint16_t)y * _width + x] = color;
  }

  static int jpegDraw(JPEGDRAW *draw) {
    if (!_owner || !draw || !draw->pPixels) return 0;
    for (int y = 0; y < draw->iHeight; y++) {
      for (int x = 0; x < draw->iWidthUsed; x++) {
        _owner->store(draw->x + x, draw->y + y, draw->pPixels[y * draw->iWidth + x], true);
      }
    }
    return 1;
  }

  static void *pngOpen(const char *name, int32_t *size) {
    if (!_owner) return nullptr;
    _owner->_file = SD.open(name, FILE_READ);
    if (!_owner->_file) return nullptr;
    *size = (int32_t)_owner->_file.size();
    return &_owner->_file;
  }

  static void pngClose(void *handle) {
    File *file = static_cast<File *>(handle);
    if (file) file->close();
  }

  static int32_t pngRead(PNGFILE *file, uint8_t *buffer, int32_t length) {
    if (!file || !file->fHandle) return 0;
    return static_cast<File *>(file->fHandle)->read(buffer, length);
  }

  static int32_t pngSeek(PNGFILE *file, int32_t position) {
    if (!file || !file->fHandle || position < 0) return 0;
    return static_cast<File *>(file->fHandle)->seek(position) ? position : 0;
  }

  static int pngDraw(PNGDRAW *draw) {
    if (!_owner || !draw || draw->iWidth <= 0) return 0;
    // PNGdec emits a complete packed/SIMD group, including up to seven
    // pixels past a non-aligned visible width.
    const size_t lineBytes = ((size_t)draw->iWidth + 7) * sizeof(uint16_t);
    uint16_t *line = static_cast<uint16_t *>(psramFound() ? ps_malloc(lineBytes) : malloc(lineBytes));
    if (!line) return 0;
    const uint16_t background = _owner->_backgroundColor;
    const uint32_t backgroundRgb = ((uint32_t)(background >> 8 & 0xf8)) |
                                   ((uint32_t)(background >> 3 & 0xfc) << 8) |
                                   ((uint32_t)(background << 3 & 0xf8) << 16);
    _owner->_png.getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, backgroundRgb);
    for (int x = 0; x < draw->iWidth; x++) _owner->store(x, draw->y, line[x], false);
    free(line);
    return 1;
  }

  static void *gifOpen(const char *name, int32_t *size) {
    return pngOpen(name, size);
  }

  static void gifClose(void *handle) {
    pngClose(handle);
  }

  static int32_t gifRead(GIFFILE *file, uint8_t *buffer, int32_t length) {
    if (!file || !file->fHandle) return 0;
    return static_cast<File *>(file->fHandle)->read(buffer, length);
  }

  static int32_t gifSeek(GIFFILE *file, int32_t position) {
    if (!file || !file->fHandle || position < 0) return 0;
    if (!static_cast<File *>(file->fHandle)->seek(position)) return 0;
    file->iPos = position;
    return position;
  }

  static void gifDraw(GIFDRAW *draw) {
    if (!_owner || !draw || !draw->pPixels || !draw->pPalette) return;
    for (int x = 0; x < draw->iWidth; x++) {
      const uint8_t paletteIndex = draw->pPixels[x];
      if (draw->ucHasTransparency && paletteIndex == draw->ucTransparent) continue;
      _owner->store(draw->iX + x, draw->iY + draw->y, draw->pPalette[paletteIndex], false);
    }
  }
};

ThumbnailDecoder *ThumbnailDecoder::_owner = nullptr;

class HttpLock {
public:
  explicit HttpLock(SemaphoreHandle_t lock, TickType_t timeout = portMAX_DELAY)
    : _lock(lock), _locked(lock && xSemaphoreTake(lock, timeout) == pdTRUE) {}
  ~HttpLock() { if (_locked) xSemaphoreGive(_lock); }
  bool locked() const { return _locked; }
  bool acquire(TickType_t timeout) {
    if (_locked) return true;
    _locked = _lock && xSemaphoreTake(_lock, timeout) == pdTRUE;
    return _locked;
  }
private:
  SemaphoreHandle_t _lock;
  bool _locked;
};
}

BrowserScreen::BrowserScreen(Navigator &navigator,
                             TouchController &touch,
                             DisplayController &display,
                             WiFiController &wifi,
                             SdCardController &sdCard)
  : Screen(navigator, touch, display),
    _wifi(wifi),
    _sdCard(sdCard),
    _transport(wifi),
    _persistentState(sdCard),
    _editingFormControl(-1),
    _historyCount(0),
    _loaded(false),
    _lifecycle(*this),
    _showStart(true),
    _showFavorites(false),
    _startListScroll(0),
    _androidUserAgent(false),
    _menuOpen(false),
    _lastTouchDown(false),
    _loadCancelled(false),
    _loadCancelTouchDown(false),
    _foregroundLoading(false),
    _page(0),
    _currentUrl(),
    _title(),
    _message(),
    _loadingPhase(),
    _loadingBytes(0),
    _loadingTotalBytes(-1),
    _loadingStartedMs(0),
    _lastLoadingDrawMs(0),
    _lastLoadingDrawBytes(0),
    _lastLoadingPercent(0),
    _lastLoadingKnownSize(false),
    _usingSessionPage(false),
    _downloadReturnUrl(),
    _deferCachedPageDraw(false),
    _deferCachedPageDrawStartedMs(0),
    _nextImageToQueue(0),
    _imageGeneration(1),
    _activeImageGeneration(0),
    _imageJobs(xQueueCreate(ImageJobQueueLength, sizeof(ImageJob))),
    _imageResults(xQueueCreate(ImageJobQueueLength, sizeof(ImageResult))),
    _imageWorker(nullptr),
    _closeBounds{0, 0, UiKit::AppBarHeight, UiKit::AppBarHeight},
    _backBounds{0, 0, 0, 0},
    _homeBounds{0, 0, 0, 0},
    _startAddBounds{0, 0, 0, 0},
    _startToggleBounds{0, 0, 0, 0},
    _startPreviousBounds{0, 0, 0, 0},
    _startNextBounds{0, 0, 0, 0},
    _previousBounds{0, 0, 0, 0},
    _nextBounds{0, 0, 0, 0},
    _menuBounds{0, 0, 0, 0},
    _menuAgentBounds{0, 0, 0, 0},
    _menuFavoriteBounds{0, 0, 0, 0} {
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    _startSiteBounds[i] = {0, 0, 0, 0};
    _startRemoveBounds[i] = {0, 0, 0, 0};
  }
  resetSessionCacheEntries();
  if (_imageJobs && _imageResults) {
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    const BaseType_t workerCore = 0;
#else
    const BaseType_t workerCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
    xTaskCreatePinnedToCore(imageWorkerTask, "browser-images", 16384, this, tskIDLE_PRIORITY + 1,
                            &_imageWorker, workerCore);
    browserLog(String(F("image worker started on core ")) + workerCore);
  } else {
    browserLog(F("image worker unavailable: queue allocation failed"));
  }
  {
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    const BaseType_t workerCore = 0;
#else
    const BaseType_t workerCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
    // This worker only prepares disposable cache data and saved state.  It
    // must never outrun the core's idle task: on the usual Arduino dual-core
    // setup it is pinned to CPU 0, whose idle task feeds the task watchdog.
    if (!_lifecycle.begin(workerCore)) browserLog(F("browser startup worker unavailable"));
  }
}

void BrowserScreen::onEnter() {
  browserLog(F("entered"));
  _lastTouchDown = false;
  // Draw the interactive shell before any SD work.  The lifecycle worker
  // loads persistent state and clears the previous session cache on the
  // other core; this avoids holding the Home screen during Browser entry.
  if ((!_sessionCacheActive || !_loaded) && !_lifecycle.loading()) startLifecycleLoad();
  draw();
}

void BrowserScreen::onResume() {
  _lastTouchDown = false;
  if (_downloadReturnUrl.length() == 0) {
    onEnter();
    return;
  }

  const String sourceUrl = _downloadReturnUrl;
  _downloadReturnUrl = "";
  browserLog(String(F("restoring download source from session cache: ")) + sourceUrl);
  // A download is not a browser page.  Reopen the document that initiated it
  // without changing history; openUrl() uses the retained session copy first
  // and only falls back to the network if that copy is unavailable.
  if (!openUrl(sourceUrl, false, false, true)) {
    browserLog(F("could not restore download source"));
  }
}

void BrowserScreen::onResult(const ScreenResult &result) {
  if (_editingFormControl >= 0) {
    if (result.kind == ScreenResultKind::Text &&
        _editingFormControl < _document.formControlCount()) {
      if (_document.setFormValue(_editingFormControl, result.text)) draw();
    }
    _editingFormControl = -1;
    return;
  }
  if (result.kind == ScreenResultKind::Text && result.text.length() > 0) {
    browserLog(F("received URL from text input"));
    openUrl(result.text, true, true);
  }
}

void BrowserScreen::tick(uint32_t nowMs) {
  (void)nowMs;
  if (_lifecycle.takeDrawPending()) {
    draw();
  }
  collectImageResults();
  pumpImageJobs();
  if (_deferCachedPageDraw &&
      (cachedPageImagesComplete() || millis() - _deferCachedPageDrawStartedMs >= 1000)) {
    const bool imagesComplete = cachedPageImagesComplete();
    _deferCachedPageDraw = false;
    reflowDocument();
    finishLoading();
    browserLog(imagesComplete ? F("cached page images ready; drawing page")
                              : F("cached image wait exceeded one second; drawing page"));
    draw();
  }
}

void BrowserScreen::startLifecycleLoad() {
  // User-agent override is intentionally session-only.  Starting Browser
  // after it has been closed always returns to the lightweight ESP identity.
  _androidUserAgent = false;
  _showFavorites = false;
  _startListScroll = 0;
  _menuOpen = false;
  // Do not touch the SD card here.  A failed queue allocation still leaves a
  // useful Browser shell; state can be loaded on a later entry.
  if (!_lifecycle.schedule()) browserLog(F("browser startup job could not be queued"));
}

void BrowserScreen::runBrowserLifecycle(uint32_t generation) {
  // A browser session never needs its old temporary pages after it has been
  // closed. This method runs only on BrowserLifecycleWorker's other-core task.
  startSession();
  {
    HttpLock lock(_sessionCacheMutex);
    if (lock.locked() && _lifecycle.isCurrent(generation))
      _sessionCacheInitialized = clearSessionCacheLocked();
  }
  if (_lifecycle.isCurrent(generation) && !_loaded) {
    // Let IDLE0 run between cleanup and state parsing on the Arduino core.
    vTaskDelay(1);
    loadState();
    _loaded = true;
  }
  if (_lifecycle.isCurrent(generation)) browserLog(F("browser startup work complete"));
}

void BrowserScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

  if (_menuOpen) {
    if (contains(_menuAgentBounds, point.x, point.y)) {
      toggleUserAgent();
      return;
    }
    if (contains(_menuFavoriteBounds, point.x, point.y)) {
      toggleFavorite();
      return;
    }
    _menuOpen = false;
    draw();
    return;
  }

  if (contains(_closeBounds, point.x, point.y)) {
    browserLog(F("close requested"));
    releaseEmbeddedImages();
    // Never wait for an image decoder or delete a directory from the touch
    // handler.  The next Browser entry clears the disposable session cache
    // from its lifecycle worker.
    stopNetworkClientsIfIdle();
    endSessionCache();
    _historyCount = 0;
    _currentUrl = "";
    _downloadReturnUrl = "";
    _title = "";
    _message = "";
    _document.clear();
    _page = 0;
    _showStart = true;
    _showFavorites = false;
    _startListScroll = 0;
    _menuOpen = false;
    _navigator.pop();
    return;
  }
  if (!_showStart && contains(_menuBounds, point.x, point.y)) {
    _menuOpen = true;
    draw();
    return;
  }
  if (!_showStart && contains(_backBounds, point.x, point.y)) {
    if (isLoading()) {
      browserLog(F("cancel requested"));
      cancelLoading();
    } else if (!_showStart) {
      browserLog(F("back requested"));
      goBack();
    }
    return;
  }
  if (!_showStart && contains(_homeBounds, point.x, point.y)) {
    browserLog(F("returning to start page"));
    if (isLoading()) releaseEmbeddedImages();
    _showStart = true;
    _page = 0;
    _message = "";
    _showFavorites = false;
    _startListScroll = 0;
    _menuOpen = false;
    draw();
    return;
  }
  if (_showStart && contains(_startAddBounds, point.x, point.y)) {
    browserLog(F("opening URL input"));
    openUrlInput();
    return;
  }
  if (_showStart && contains(_startToggleBounds, point.x, point.y)) {
    _showFavorites = !_showFavorites;
    _startListScroll = 0;
    browserLog(_showFavorites ? F("showing favorite sites") : F("showing recent sites"));
    draw();
    return;
  }
  if (_showStart && !_lifecycle.loading() && contains(_startPreviousBounds, point.x, point.y)) {
    if (_startListScroll > 0) {
      _startListScroll -= StartListVisibleRows;
      draw();
    }
    return;
  }
  if (_showStart && !_lifecycle.loading() && contains(_startNextBounds, point.x, point.y)) {
    const uint8_t count = startSiteCount();
    if (_startListScroll + StartListVisibleRows < count) {
      _startListScroll += StartListVisibleRows;
      draw();
    }
    return;
  }
  if (!_showStart && contains(_previousBounds, point.x, point.y)) {
    if (_page > 0) {
      _page--;
      browserLog(String(F("moved to page ")) + String(_page + 1));
      redrawDocument();
      drawPageControls();
    }
    return;
  }
  if (!_showStart && contains(_nextBounds, point.x, point.y)) {
    if (hasNextPage()) {
      _page++;
      browserLog(String(F("moved to page ")) + String(_page + 1));
      redrawDocument();
      drawPageControls();
    }
    return;
  }
  if (_showStart) {
    for (uint8_t i = 0; i < MaxRecentSites; i++) {
      if (contains(_startSiteBounds[i], point.x, point.y)) {
        const RecentSite &site = startSiteAt(i);
        if (contains(_startRemoveBounds[i], point.x, point.y)) {
          if (_showFavorites) {
            browserLog(String(F("removing favorite site #")) + String(i + 1));
            removeFavorite(site.url);
          } else {
            browserLog(String(F("removing recent site #")) + String(i + 1));
            removeRecent(i);
          }
          saveState();
          draw();
          return;
        }
        browserLog(String(_showFavorites ? F("opening favorite site #") : F("opening recent site #")) + String(i + 1));
        const String url = site.url;
        const String title = site.title;
        if (!_showFavorites) {
          // Promote immediately so the tap is reflected even if the reload
          // fails; a successful load below refreshes this title from the page.
          addRecent(url, title);
          saveState();
        }
        openUrl(url, true, !_showFavorites);
        return;
      }
    }
  }
  if (!_showStart) {
    int8_t formControl = documentFormControlAt(point.x, point.y);
    if (formControl >= 0) {
      handleFormControl((uint8_t)formControl);
      return;
    }
    String linkUrl = documentLinkAt(point.x, point.y);
    if (linkUrl.length()) {
      browserLog(String(F("link tapped: current=")) + _currentUrl + F(", target=") + linkUrl +
                 F(", targetIsCurrent=") + (linkUrl == _currentUrl ? F("yes") : F("no")));
      openUrl(linkUrl);
    }
  }
}

void BrowserScreen::layout() {
  const int16_t width = _display.width();
  const int16_t height = _display.height();
  const int16_t buttonWidth = width / 4;
  _closeBounds = {0, 0, UiKit::AppBarHeight, UiKit::AppBarHeight};
  _backBounds = {0, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _homeBounds = {buttonWidth, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _startAddBounds = {0, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _startToggleBounds = {buttonWidth, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _startPreviousBounds = {(int16_t)(buttonWidth * 2), (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _startNextBounds = {(int16_t)(buttonWidth * 3), (int16_t)(height - ControlsHeight),
                      (int16_t)(width - buttonWidth * 3), ControlsHeight};
  _previousBounds = {(int16_t)(buttonWidth * 2), (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _nextBounds = {(int16_t)(buttonWidth * 3), (int16_t)(height - ControlsHeight),
                 (int16_t)(width - buttonWidth * 3), ControlsHeight};
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    _startSiteBounds[i] = {0, 0, 0, 0};
    _startRemoveBounds[i] = {0, 0, 0, 0};
  }
  clampStartListScroll();
  const uint8_t count = _lifecycle.loading() ? 0 : startSiteCount();
  const uint8_t visible = count - _startListScroll < StartListVisibleRows
    ? count - _startListScroll : StartListVisibleRows;
  for (uint8_t row = 0; row < visible; row++) {
    const uint8_t index = _startListScroll + row;
    _startSiteBounds[index] = {0, (int16_t)(RecentListTop + row * UiKit::ListCellHeight),
                        width, UiKit::ListCellHeight};
    _startRemoveBounds[index] = {(int16_t)(width - UiKit::ListCellHeight),
                                 (int16_t)(RecentListTop + row * UiKit::ListCellHeight),
                                 UiKit::ListCellHeight, UiKit::ListCellHeight};
  }
  _menuBounds = {(int16_t)(width - UiKit::NavigationIconSize - UiKit::AppBarIconPadding),
                 UiKit::AppBarIconPadding,
                 UiKit::NavigationIconSize, UiKit::NavigationIconSize};
  const int16_t menuWidth = width > 240 ? 224 : width - 16;
  const int16_t menuX = width - menuWidth - 8;
  _menuAgentBounds = {menuX, (int16_t)(HeaderHeight + 6), menuWidth, UiKit::ListCellHeight};
  _menuFavoriteBounds = {menuX, (int16_t)(HeaderHeight + 6 + UiKit::ListCellHeight),
                         menuWidth, UiKit::ListCellHeight};
}

BrowserDocumentLayout BrowserScreen::documentLayout() const {
  return {
    _display.width(),
    static_cast<int16_t>(HeaderHeight + DocumentPadding),
    static_cast<int16_t>(_display.height() - ControlsHeight - 4),
    DocumentSidePadding,
    TextRowHeight,
    FormControlHeight,
    FormTextareaHeight,
    imageWidth("")
  };
}

void BrowserScreen::draw() {
  if (!_display.isReady()) return;
  BrowserDisplayLock lock;
  if (!lock.locked()) {
    browserLog(F("display update deferred: SPI bus is busy"));
    return;
  }
  layout();
  TFT_eSPI &tft = _display.tft();
  BrowserText::begin(tft);
  tft.fillScreen(TFT_BLACK);
  drawHeader();
  if (_showStart) drawStartPage();
  else drawDocument();
  drawControls();
  if (_menuOpen && !_showStart) drawPageMenu();
  BrowserText::end(tft);
}

void BrowserScreen::drawHeader() {
  String heading = _showStart ? "Browser" : (_title.length() ? _title : _currentUrl);
  TFT_eSPI &tft = _display.tft();
  UiKit::drawTopAppBar(tft, _display.width(), "");
  const int16_t titleWidth = _display.width() - (_showStart ? 72 : 120);
  BrowserText::draw(tft, BrowserText::ellipsize(tft, heading, titleWidth,
                                                 BrowserText::Size::Title),
                    UiKit::AppBarHeight, 9, TFT_WHITE,
                    UiKit::AppBarColor, BrowserText::Size::Title);
  if (!_showStart) {
    UiKit::drawMoreIcon(tft, _menuBounds.x + _menuBounds.w / 2,
                        _menuBounds.y + _menuBounds.h / 2,
                        TFT_WHITE, UiKit::AppBarColor);
  }
}

void BrowserScreen::drawStartPage() {
  TFT_eSPI &tft = _display.tft();
  UiText::draw(tft, _showFavorites ? "Favorite sites" : "Recent sites", 10, RecentListTop - 18, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact);
  // Recent-site records are populated by the lifecycle worker.  The footer
  // remains usable while it works, but the list itself is read only after its
  // hand-off frame in tick().
  if (_lifecycle.loading()) {
    UiText::draw(tft, "Loading saved browser data...", 10, RecentListTop + 10,
                 TFT_DARKGREY, TFT_BLACK, UiText::Size::Compact);
    return;
  }
  const uint8_t count = startSiteCount();
  if (count == 0) {
    UiText::draw(tft, _showFavorites ? "No favorite sites yet" : "No recent sites yet", 10, RecentListTop + 10,
                 TFT_DARKGREY, TFT_BLACK, UiText::Size::Compact);
    return;
  }
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    if (_startSiteBounds[i].w == 0) continue;
    const RecentSite &site = startSiteAt(i);
    const Rect &bounds = _startSiteBounds[i];
    const Rect &removeBounds = _startRemoveBounds[i];
    String label = site.title.length() ? site.title : site.url;
    tft.fillRect(0, bounds.y, bounds.w, UiKit::ListCellHeight, TFT_BLACK);
    BrowserText::draw(tft, BrowserText::ellipsize(tft, label, bounds.w - UiKit::ContentPadding - removeBounds.w - 6),
                      UiKit::ContentPadding, bounds.y + 10, TFT_WHITE, TFT_BLACK,
                      BrowserText::Size::Content);
    UiKit::drawListCloseIcon(tft, removeBounds.x + removeBounds.w / 2,
                             removeBounds.y + removeBounds.h / 2,
                             TFT_LIGHTGREY, TFT_BLACK);
    tft.drawFastHLine(UiKit::ContentPadding, bounds.y + UiKit::ListCellHeight - 1,
                      bounds.w - UiKit::ContentPadding * 2, UiKit::DividerColor);
  }
}

void BrowserScreen::drawPageMenu() {
  TFT_eSPI &tft = _display.tft();
  const int16_t menuX = _menuAgentBounds.x;
  const int16_t menuY = _menuAgentBounds.y;
  const int16_t menuWidth = _menuAgentBounds.w;
  const int16_t menuHeight = _menuAgentBounds.h + _menuFavoriteBounds.h;
  tft.fillRoundRect(menuX, menuY, menuWidth, menuHeight, 4, TFT_BLACK);
  tft.drawRoundRect(menuX, menuY, menuWidth, menuHeight, 4, TFT_DARKGREY);
  UiText::draw(tft, "Override User-Agent", menuX + UiKit::ContentPadding,
               _menuAgentBounds.y + UiKit::ListCellHeight / 2, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, ML_DATUM);
  drawCheckbox(tft, menuX + menuWidth - UiKit::ContentPadding - 16,
               _menuAgentBounds.y + (_menuAgentBounds.h - 16) / 2,
               _androidUserAgent, TFT_WHITE);
  tft.drawFastHLine(menuX + UiKit::ContentPadding, _menuFavoriteBounds.y,
                    menuWidth - UiKit::ContentPadding * 2, UiKit::DividerColor);
  UiText::draw(tft, isFavorite(_currentUrl) ? "Remove from favorites" : "Add to favorites",
               menuX + UiKit::ContentPadding, _menuFavoriteBounds.y + UiKit::ListCellHeight / 2,
               TFT_WHITE, TFT_BLACK, UiText::Size::Compact, ML_DATUM);
}

void BrowserScreen::drawDocument() {
  TFT_eSPI &tft = _display.tft();
  const uint16_t documentBackground = _foregroundLoading ? TFT_WHITE : _document.backgroundColor();
  tft.fillRect(0, HeaderHeight, _display.width(),
               _display.height() - HeaderHeight - ControlsHeight, documentBackground);
  const int16_t textTop = HeaderHeight + DocumentPadding;
  if (_foregroundLoading) {
    drawLoadingPanel(true, true);
    return;
  }
  if (_message.length() > 0) {
    const String hint = shortText(_message, _display.width() - 28);
    const int16_t hintWidth = UiText::width(tft, hint, UiText::Size::Compact) + 14;
    const int16_t hintX = (_display.width() - hintWidth) / 2;
    const uint16_t hintBackground = TFT_RED;
    const uint16_t hintForeground = TFT_WHITE;
    tft.fillRoundRect(hintX, textTop + 7, hintWidth, 26, 5, hintBackground);
    UiText::draw(tft, hint, hintX + 7, textTop + 12, hintForeground, hintBackground,
                 UiText::Size::Compact);
    return;
  }
  if (_document.rowCount() == 0) {
    UiText::draw(tft, "Page has no readable text", 10, textTop,
                 TFT_DARKGREY, documentBackground, UiText::Size::Compact);
    return;
  }
  const uint16_t start = _document.pageStart(_page);
  const uint16_t end = _document.pageEnd(_page);
  int16_t y = textTop;
  for (uint16_t index = start; index < end; index++) {
    const VisualRow *rowRecord = _document.row(index);
    if (!rowRecord) break;
    const VisualRow &row = *rowRecord;
    const int16_t height = _document.rowHeight(index, documentLayout());
    if (row.formControlIndex >= 0) {
      drawFormControl(index, y);
    } else if (row.image) {
      uint16_t color = row.imageLink ? LinkImageColor : ImagePlaceholderColor;
      const uint16_t rowBackground = row.style.hasBlockBackground ? row.style.blockBackgroundColor : documentBackground;
      int16_t width = imageWidth(row.imageLabel);
      const EmbeddedImage *image = row.imageIndex >= 0 ? _document.image(row.imageIndex) : nullptr;
      const bool hasThumbnail = image && image->pixels;
      if (hasThumbnail) {
        width = image->width;
      }
      if (row.style.hasBlockBackground) {
        tft.fillRect(DocumentSidePadding, y, _display.width() - DocumentSidePadding * 2, height,
                     row.style.blockBackgroundColor);
      } else if (row.style.hasBackground) {
        tft.fillRect(DocumentSidePadding, y, width, height, row.style.backgroundColor);
      }
      if (hasThumbnail) {
        drawThumbnail(row.imageIndex, DocumentSidePadding, y);
      } else {
        tft.drawXBitmap(DocumentSidePadding + 2, y + 2, ic_file, 16, 16, color);
        BrowserText::draw(tft, ImagePlaceholderLabel,
                          DocumentSidePadding + 22, y + 3, color, rowBackground);
      }
    } else {
      const uint16_t rowBackground = row.style.hasBlockBackground ? row.style.blockBackgroundColor : documentBackground;
      if (row.style.hasBlockBackground) {
        tft.fillRect(DocumentSidePadding, y, _display.width() - DocumentSidePadding * 2, height,
                     row.style.blockBackgroundColor);
      }
      for (uint8_t runIndex = 0; runIndex < row.runCount; runIndex++) {
        const VisualRun &run = row.runs[runIndex];
        const int16_t x = DocumentSidePadding + run.x;
        if (run.image) {
          const EmbeddedImage *image = run.imageIndex >= 0 ? _document.image(run.imageIndex) : nullptr;
          const bool hasDirectImageLink = image != nullptr;
          uint16_t color = (run.style.linkIndex || hasDirectImageLink) ? LinkImageColor : ImagePlaceholderColor;
          const int16_t imageHeight = image && image->pixels ? image->height : TextRowHeight;
          if (run.style.hasBackground) tft.fillRect(x, y, run.width, imageHeight, run.style.backgroundColor);
          const bool hasThumbnail = image && image->pixels;
          if (hasThumbnail) {
            drawThumbnail(run.imageIndex, x, y);
          } else {
            tft.drawXBitmap(x + 2, y + 3, ic_file, 16, 16, color);
            BrowserText::draw(tft, ImagePlaceholderLabel, x + 22, y + 3, color,
                              run.style.hasBackground ? run.style.backgroundColor : rowBackground, run.style.size);
          }
          continue;
        }
        if (run.style.hasBackground && run.width > 0) {
          tft.fillRect(x, y, run.width, height, run.style.backgroundColor);
        }
        const int16_t textY = y + (height - BrowserText::lineHeight(run.style.size)) / 2;
        BrowserText::draw(tft, run.text, x, textY, run.style.color,
                          run.style.hasBackground ? run.style.backgroundColor : rowBackground, run.style.size);
        if (run.style.underline && run.width > 0) {
          tft.drawFastHLine(x, y + height - 2, run.width, run.style.color);
        }
      }
    }
    y += height;
  }
}

void BrowserScreen::drawLoadingPanel(bool redrawPhase, bool drawBarFrame) {
  TFT_eSPI &tft = _display.tft();
  const int16_t documentHeight = _display.height() - HeaderHeight - ControlsHeight;
  const int16_t panelTop = HeaderHeight + (documentHeight - 74) / 2;
  const bool knownSize = _loadingTotalBytes > 0;
  uint8_t percent = 0;
  if (knownSize) {
    percent = (uint8_t)((uint64_t)_loadingBytes * 100 / (uint32_t)_loadingTotalBytes);
    if (percent > 100) percent = 100;
  }
  const String detail = knownSize
    ? String(percent) + F("%  ") + String(_loadingBytes / 1024) + F(" / ") +
      String(((uint32_t)_loadingTotalBytes + 1023) / 1024) + F(" KB")
    : (_loadingBytes ? String(_loadingBytes / 1024) + F(" KB received") : String(F("Please wait")));
  const int16_t barX = 18;
  const int16_t barY = panelTop + 54;
  const int16_t barWidth = _display.width() - barX * 2;

  if (redrawPhase) {
    tft.fillRect(0, panelTop, _display.width(), 27, TFT_WHITE);
    const String phase = shortText(_loadingPhase.length() ? _loadingPhase : String(F("Loading")),
                                   _display.width() - 28);
    UiText::draw(tft, phase, _display.width() / 2, panelTop + 18,
                 TFT_BLACK, TFT_WHITE, UiText::Size::Compact, MC_DATUM);
  }
  // Only this short line and the bar interior change during a transfer. This
  // leaves the loading title and the bar frame on screen, eliminating flicker.
  tft.fillRect(0, panelTop + 28, _display.width(), 21, TFT_WHITE);
  UiText::draw(tft, detail, _display.width() / 2, panelTop + 39,
               TFT_DARKGREY, TFT_WHITE, UiText::Size::Compact, MC_DATUM);
  if (knownSize && _lastLoadingKnownSize && !drawBarFrame) {
    UiKit::updateProgressBar(tft, barX, barY, barWidth, 12, _lastLoadingPercent, percent,
                             TFT_BLUE, TFT_WHITE);
  } else {
    UiKit::drawProgressBar(tft, barX, barY, barWidth, 12, percent, !knownSize,
                           TFT_BLUE, TFT_DARKGREY, TFT_WHITE, drawBarFrame,
                           millis() - _loadingStartedMs);
  }
  _lastLoadingPercent = percent;
  _lastLoadingKnownSize = knownSize;
}

void BrowserScreen::redrawDocument() {
  if (!_display.isReady()) return;
  BrowserDisplayLock lock;
  if (!lock.locked()) return;
  BrowserText::begin(_display.tft());
  drawDocument();
  BrowserText::end(_display.tft());
}

void BrowserScreen::drawControls() {
  TFT_eSPI &tft = _display.tft();
  if (_showStart) {
    const Rect buttons[] = {_startAddBounds, _startToggleBounds, _startPreviousBounds, _startNextBounds};
    const uint8_t count = _lifecycle.loading() ? 0 : startSiteCount();
    // Opening a URL and switching between the two lists do not inspect the
    // records being loaded on the other core, so keep those controls live.
    // Paging waits for the completed, consistent list hand-off.
    const bool active[] = {true, true, !_lifecycle.loading() && _startListScroll > 0,
                           !_lifecycle.loading() && _startListScroll + StartListVisibleRows < count};
    for (uint8_t i = 0; i < 4; i++) {
      const Rect &button = buttons[i];
      const uint16_t color = FooterButtonColor;
      const uint16_t iconColor = active[i] ? TFT_WHITE : 0x8410;
      tft.fillRect(button.x, button.y, button.w - 1, button.h, color);
      const int16_t centerX = button.x + button.w / 2;
      const int16_t centerY = button.y + button.h / 2;
      if (i == 0) UiKit::drawPlusIcon(tft, centerX, centerY, iconColor, color);
      else if (i == 1) {
        if (_showFavorites) UiKit::drawRecentIcon(tft, centerX, centerY, iconColor, color);
        else UiKit::drawFavoriteIcon(tft, centerX, centerY, iconColor, color);
      } else {
        UiKit::drawPageArrow(tft, centerX, centerY, true, i == 3, iconColor, color);
      }
    }
    tft.setTextDatum(TL_DATUM);
    return;
  }
  const Rect buttons[] = {_backBounds, _homeBounds, _previousBounds, _nextBounds};
  const bool loading = isLoading();
  for (uint8_t i = 0; i < 4; i++) {
    bool active = (i == 0 && (loading || !_showStart)) || (i == 1 && !_showStart) ||
      (!_showStart && ((i == 2 && _page > 0) ||
      (i == 3 && hasNextPage())));
    const uint16_t color = FooterButtonColor;
    const Rect &button = buttons[i];
    tft.fillRect(button.x, button.y, button.w - 1, button.h, color);
    uint16_t iconColor = active ? TFT_WHITE : 0x8410;
    const int16_t centerX = button.x + button.w / 2;
    const int16_t centerY = button.y + button.h / 2;
    if (i == 0 && loading) UiKit::drawCloseIcon(tft, centerX, centerY, iconColor, color);
    else if (i == 0) UiKit::drawBackIcon(tft, centerX, centerY, iconColor, color);
    else if (i == 1) UiKit::drawHomeIcon(tft, centerX, centerY, iconColor, color);
    else UiKit::drawPageArrow(tft, centerX, centerY, true, i == 3, iconColor, color);
  }
  tft.setTextDatum(TL_DATUM);
}

void BrowserScreen::drawPageControls() {
  BrowserDisplayLock lock;
  if (!lock.locked()) return;
  TFT_eSPI &tft = _display.tft();
  const Rect buttons[] = {_previousBounds, _nextBounds};
  for (uint8_t index = 0; index < 2; index++) {
    const bool active = index == 0 ? _page > 0 : hasNextPage();
    const Rect &button = buttons[index];
    tft.fillRect(button.x, button.y, button.w - 1, button.h, FooterButtonColor);
    UiKit::drawPageArrow(tft, button.x + button.w / 2, button.y + button.h / 2,
                         true, index == 1, active ? TFT_WHITE : 0x8410,
                         FooterButtonColor);
  }
  tft.setTextDatum(TL_DATUM);
}

bool BrowserScreen::contains(const Rect &rect, int16_t x, int16_t y) const {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

bool BrowserScreen::isLoading() const {
  if (_foregroundLoading) return true;
  if (_activeImageGeneration == _imageGeneration) return true;
  return _document.hasUnfinishedImages(_imageGeneration);
}

void BrowserScreen::beginLoading(const String &phase) {
  _foregroundLoading = true;
  _loadingBytes = 0;
  _loadingTotalBytes = -1;
  _loadingStartedMs = millis();
  _lastLoadingDrawMs = 0;
  _lastLoadingDrawBytes = 0;
  _lastLoadingPercent = 0;
  _lastLoadingKnownSize = false;
  _loadingPhase = phase;
  browserLog(String(F("progress: ")) + phase);
}

void BrowserScreen::setLoadingPhase(const String &phase, int32_t totalBytes) {
  // Image downloads happen on the second core. Their results redraw the
  // document in tick(), while this progress panel is reserved for the
  // foreground navigation or direct file transfer.
  if (!_foregroundLoading || _activeImageGeneration) return;
  const bool changed = _loadingPhase != phase || _loadingTotalBytes != totalBytes;
  _loadingPhase = phase;
  _loadingTotalBytes = totalBytes;
  if (changed) {
    browserLog(String(F("progress: ")) + phase +
               (totalBytes >= 0 ? String(F(", expected bytes=")) + String(totalBytes) : String()));
    refreshLoadingDisplay(true);
  }
}

void BrowserScreen::advanceLoading(uint32_t bytes) {
  if (!_foregroundLoading || _activeImageGeneration || bytes == 0) return;
  _loadingBytes += bytes;
  refreshLoadingDisplay();
}

void BrowserScreen::finishLoading() {
  if (!_foregroundLoading) return;
  browserLog(String(F("progress complete: ")) + _loadingPhase + F(", bytes=") + String(_loadingBytes));
  _foregroundLoading = false;
  _loadingPhase = "";
  _loadingTotalBytes = -1;
}

void BrowserScreen::refreshLoadingDisplay(bool force) {
  if (!_foregroundLoading || !_display.isReady()) return;
  const uint32_t now = millis();
  // Drawing each received byte would make slow responses slower. Refresh at
  // most eight times a second, while still updating immediately after a
  // phase change or every 4 KiB on fast transfers.
  if (!force && now - _lastLoadingDrawMs < 125 && _loadingBytes - _lastLoadingDrawBytes < 4096) return;
  _lastLoadingDrawMs = now;
  _lastLoadingDrawBytes = _loadingBytes;
  BrowserDisplayLock lock;
  if (!lock.locked()) return;
  drawLoadingPanel(force, false);
}

void BrowserScreen::cancelLoading() {
  const bool contentIsPending = _foregroundLoading;
  browserLog(F("cancelling active transfer and image jobs"));
  releaseEmbeddedImages();
  _deferCachedPageDraw = false;
  // The image worker performs HTTPS on the other core. Calling stop() while
  // it is inside mbedTLS races its context teardown and corrupts the heap.
  // Invalidating the generation makes the worker stop its own client at its
  // next cancellation check; only stop here after exclusively acquiring the
  // same lock used for every request.
  stopNetworkClientsIfIdle();
  _loadCancelled = true;
  finishLoading();
  if (contentIsPending) _message = "Loading cancelled";
  draw();
}

void BrowserScreen::stopNetworkClientsIfIdle() {
  HttpLock lock(_transport.mutex(), 0);
  if (!lock.locked()) {
    browserLog(F("network client stop deferred to active request"));
    return;
  }
  _transport.stopClients();
}

bool BrowserScreen::pollLoadingCancel() {
  // The foreground HTTP request runs in the Arduino task, so it needs to
  // poll touch itself. Image work is already on the second core and is
  // cancelled by releaseEmbeddedImages() through its generation check.
  // Do not infer the caller from _activeImageGeneration: a newly opened link
  // can start while a cancelled image worker is still unwinding. In that
  // case the foreground request must inspect touch state, not inherit the
  // worker's stale generation as a cancellation.
  const bool calledByImageWorker = _imageWorker && xTaskGetCurrentTaskHandle() == _imageWorker;
  if (calledByImageWorker) {
    const bool cancelled = _activeImageGeneration != _imageGeneration;
    if (cancelled) {
      browserLog(String(F("image worker cancelled: activeGeneration=")) + String(_activeImageGeneration) +
                 F(", currentGeneration=") + String(_imageGeneration));
    }
    return cancelled;
  }
  if (!_foregroundLoading) return _loadCancelled;
  _touch.update();
  TouchPoint point = _touch.point();
  if (!point.touched) {
    _loadCancelTouchDown = false;
    return _loadCancelled;
  }
  if (!_loadCancelTouchDown) {
    _loadCancelTouchDown = true;
    if (contains(_backBounds, point.x, point.y)) {
      browserLog(String(F("foreground cancellation requested: x=")) + String(point.x) + F(", y=") + String(point.y));
      cancelLoading();
    }
  }
  return _loadCancelled;
}

String BrowserScreen::documentLinkAt(int16_t x, int16_t y) const {
  return _message.length() ? String() : _document.linkAt(_page, x, y, documentLayout());
}

int8_t BrowserScreen::documentFormControlAt(int16_t x, int16_t y) const {
  return _message.length() ? -1 : _document.formAt(_page, x, y, documentLayout());
}

void BrowserScreen::openFormTextInput(uint8_t index) {
  const FormControl *control = _document.formControl(index);
  if (!control) return;
  String title = "Edit " + _document.formControlLabel(index);
  if (_navigator.push(ScreenId::TextInput,
                      ScreenData::textInput(shortText(title, 120), false, control->value))) {
    _editingFormControl = index;
  }
}

void BrowserScreen::resetForm(uint8_t formIndex) {
  if (_document.resetForm(formIndex)) draw();
}

void BrowserScreen::handleFormControl(uint8_t index) {
  const FormControl *control = _document.formControl(index);
  if (!control || control->disabled || control->kind == FormControlKind::Unsupported) return;
  switch (control->kind) {
    case FormControlKind::Text:
    case FormControlKind::Textarea:
      openFormTextInput(index);
      return;
    case FormControlKind::Checkbox:
      if (_document.toggleFormControl(index)) draw();
      return;
    case FormControlKind::Radio:
      if (_document.toggleFormControl(index)) draw();
      return;
    case FormControlKind::Select:
      if (_document.toggleFormControl(index)) draw();
      return;
    case FormControlKind::Reset:
      if (control->formIndex >= 0) resetForm((uint8_t)control->formIndex);
      return;
    case FormControlKind::Submit:
      if (control->formIndex >= 0) submitForm((uint8_t)control->formIndex, index);
      else {
        _message = "Submit control is outside a form";
        draw();
      }
      return;
    default:
      return;
  }
}

void BrowserScreen::submitForm(uint8_t formIndex, int8_t submitControlIndex) {
  const FormData *form = _document.form(formIndex);
  if (!form) return;
  if (form->method == FormMethod::Unsupported) {
    _message = "This form method is unsupported";
    draw();
    return;
  }
  if (!form->urlEncoded) {
    _message = "This form encoding is unsupported";
    draw();
    return;
  }
  Url base;
  if (!parseUrl(_currentUrl, base)) {
    _message = "Form action could not be resolved";
    draw();
    return;
  }
  String target = resolveUrl(base, form->action);
  String payload;
  const uint16_t maxBytes = form->method == FormMethod::Post ? MaxFormPostBytes : 0;
  if (!_document.buildFormPayload(formIndex, submitControlIndex, payload, maxBytes)) {
    _message = "Form data is too large";
    draw();
    return;
  }
  if (form->method == FormMethod::Get) {
    if (payload.length()) target += target.indexOf('?') >= 0 ? '&' : '?';
    target += payload;
    browserLog(String(F("submitting GET form: ")) + target);
    openUrl(target, true, false);
    return;
  }
  browserLog(String(F("submitting POST form: target=")) + target + F(", bodyBytes=") + String(payload.length()));
  openRequest({HttpMethod::Post, target, payload}, true, false, false);
}

void BrowserScreen::openUrlInput() {
  _navigator.push(ScreenId::TextInput, ScreenData::textInput("Open URL", false));
}

void BrowserScreen::resetSessionCacheEntries() {
  resetCacheState();
}

void BrowserScreen::endSessionCache() {
  // This is called from the close button.  Cache cleanup can include many
  // FAT deletes, and an image worker can still own one of those files, so
  // leave cleanup for startLifecycleLoad() instead of blocking the UI task.
  _lifecycle.cancel();
  endSession();
  _usingSessionPage = false;
  browserLog(F("session cache cleanup deferred"));
}

bool BrowserScreen::initializeSessionCacheLocked() {
  if (!_sessionCacheActive || !_sdCard.isMounted()) return false;
  if (_sessionCacheInitialized) return true;
  // A navigation made during the first displayed Browser frame should still
  // work, but it must not steal the cache's potentially long setup from the
  // UI task.  It simply proceeds without a session-cache write this time.
  if (_lifecycle.loading()) return false;
  _sessionCacheInitialized = clearSessionCacheLocked();
  return _sessionCacheInitialized;
}

bool BrowserScreen::clearSessionCacheLocked() {
  resetSessionCacheEntries();
  if (!_sdCard.isMounted() || !SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  File directory = SD.open(SessionCacheDirectory, FILE_READ);
  const bool isDirectory = directory && directory.isDirectory();
  SpiBusLock::unlock();
  if (isDirectory) {
    while (true) {
      if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
      File item = directory.openNextFile();
      String path = item ? String(item.name()) : String();
      if (item) item.close();
      SpiBusLock::unlock();
      if (path.length() == 0) break;
      // FS implementations differ: some report a full path here while
      // others return only the filename.  Always remove the actual session
      // entry, otherwise stale page-N.html files survive a new session.
      if (!path.startsWith("/")) {
        path = path.startsWith(".system/") ? "/" + path : String(SessionCacheDirectory) + "/" + path;
      }

      if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
      SD.remove(path.c_str());
      SpiBusLock::unlock();
      // Let the display and touch task use the shared bus between FAT
      // updates. This cleanup happens on the lifecycle worker, but holding
      // SPI during the entire directory walk still made Browser look frozen.
      vTaskDelay(1);
    }
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
    directory.close();
    SD.rmdir(SessionCacheDirectory);
    SpiBusLock::unlock();
  } else if (directory) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
    directory.close();
    SD.remove(SessionCacheDirectory);
    SpiBusLock::unlock();
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  const bool ready = (SD.exists("/.system") || SD.mkdir("/.system")) &&
                     (SD.exists(SessionCacheDirectory) || SD.mkdir(SessionCacheDirectory));
  SpiBusLock::unlock();
  return ready;
}

bool BrowserScreen::removeSessionFileLocked(const String &path) {
  if (path.length() == 0) return true;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  const bool removed = !SD.exists(path.c_str()) || SD.remove(path.c_str());
  SpiBusLock::unlock();
  return removed;
}

void BrowserScreen::removeSessionPageLocked(uint8_t index) {
  if (index >= MaxSessionPages || !_sessionPages[index].used) return;
  if (_sessionCacheBytes >= _sessionPages[index].bytes) _sessionCacheBytes -= _sessionPages[index].bytes;
  else _sessionCacheBytes = 0;
  _sessionPages[index] = {"", "", "", 0, 0, false};
}

void BrowserScreen::removeSessionImageLocked(uint8_t index) {
  if (index >= MaxSessionImages || !_sessionImages[index].used) return;
  if (_sessionCacheBytes >= _sessionImages[index].bytes) _sessionCacheBytes -= _sessionImages[index].bytes;
  else _sessionCacheBytes = 0;
  _sessionImages[index] = {"", "", 0, 0, false};
}

bool BrowserScreen::evictOldestSessionEntryLocked() {
  bool page = false;
  int16_t index = -1;
  uint32_t oldest = 0xffffffffUL;
  for (uint8_t i = 0; i < MaxSessionPages; i++) {
    if (_sessionPages[i].used && _sessionPages[i].lastUse <= oldest) {
      page = true;
      index = i;
      oldest = _sessionPages[i].lastUse;
    }
  }
  for (uint8_t i = 0; i < MaxSessionImages; i++) {
    if (_sessionImages[i].used && _sessionImages[i].lastUse <= oldest) {
      page = false;
      index = i;
      oldest = _sessionImages[i].lastUse;
    }
  }
  if (index < 0) return false;
  const String path = page ? _sessionPages[index].path : _sessionImages[index].path;
  if (!removeSessionFileLocked(path)) return false;
  if (page) removeSessionPageLocked((uint8_t)index);
  else removeSessionImageLocked((uint8_t)index);
  browserLog(String(F("session cache evicted: ")) + path);
  return true;
}

bool BrowserScreen::evictOldestSessionImageLocked() {
  int16_t index = -1;
  uint32_t oldest = 0xffffffffUL;
  for (uint8_t i = 0; i < MaxSessionImages; i++) {
    if (_sessionImages[i].used && _sessionImages[i].lastUse <= oldest) {
      index = i;
      oldest = _sessionImages[i].lastUse;
    }
  }
  if (index < 0) return false;
  if (!removeSessionFileLocked(_sessionImages[index].path)) return false;
  removeSessionImageLocked((uint8_t)index);
  browserLog(F("session image cache evicted to retain browser pages"));
  return true;
}

bool BrowserScreen::ensureSessionCacheSpaceLocked(uint32_t bytes, bool preservePages) {
  if (bytes > MaxSessionCacheBytes) return false;
  while (_sessionCacheBytes > MaxSessionCacheBytes - bytes) {
    // Page bodies are the browser's Back cache.  Preview files are optional,
    // so never let an image download evict a page the user may navigate back
    // to.  The decoder can still use the downloaded file when the image cache
    // has no room left.
    if (preservePages ? !evictOldestSessionImageLocked() : !evictOldestSessionEntryLocked()) return false;
  }
  return true;
}

uint32_t BrowserScreen::nextSessionCacheUse() {
  _sessionCacheClock++;
  if (_sessionCacheClock == 0) _sessionCacheClock = 1;
  return _sessionCacheClock;
}

String BrowserScreen::nextSessionCachePath(const char *prefix, const char *extension) {
  _sessionCacheSequence++;
  if (_sessionCacheSequence == 0) _sessionCacheSequence = 1;
  return String(SessionCacheDirectory) + "/" + prefix + String((unsigned long)_sessionCacheSequence) + extension;
}

int8_t BrowserScreen::sessionPageIndex(const String &url) const {
  for (uint8_t i = 0; i < MaxSessionPages; i++) {
    if (_sessionPages[i].used && _sessionPages[i].url == url) return (int8_t)i;
  }
  return -1;
}

int8_t BrowserScreen::sessionImageIndex(const String &url) const {
  for (uint8_t i = 0; i < MaxSessionImages; i++) {
    if (_sessionImages[i].used && _sessionImages[i].url == url) return (int8_t)i;
  }
  return -1;
}

bool BrowserScreen::loadSessionPage(const String &url, String &body, String &contentType) {
  body = "";
  contentType = "";
  if (!_sessionCacheActive) {
    browserLog(F("session page cache miss: cache is inactive"));
    return false;
  }
  if (!_sdCard.isMounted()) {
    browserLog(F("session page cache miss: SD card is unavailable"));
    return false;
  }
  HttpLock lock(_sessionCacheMutex);
  if (!lock.locked()) {
    browserLog(F("session page cache miss: cache lock unavailable"));
    return false;
  }
  if (!initializeSessionCacheLocked()) {
    browserLog(F("session page cache miss: cache initialization failed"));
    return false;
  }
  const int8_t index = sessionPageIndex(url);
  if (index < 0) {
    browserLog(String(F("session page cache miss: no entry for ")) + url);
    return false;
  }
  SessionPageCacheEntry &entry = _sessionPages[index];
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  File file = SD.open(entry.path.c_str(), FILE_READ);
  const uint32_t size = file ? (uint32_t)file.size() : 0xffffffffUL;
  SpiBusLock::unlock();
  if (!file || size != entry.bytes || size > MaxBodyBytes || !body.reserve(size)) {
    browserLog(String(F("session page cache entry invalid: url=")) + url + F(", path=") + entry.path +
               F(", expectedBytes=") + String(entry.bytes) + F(", actualBytes=") + String(size));
    if (file) {
      SpiBusLock::lock();
      file.close();
      SpiBusLock::unlock();
    }
    if (removeSessionFileLocked(entry.path)) removeSessionPageLocked((uint8_t)index);
    return false;
  }
  uint8_t buffer[512];
  uint32_t offset = 0;
  bool readOk = true;
  while (offset < size) {
    const uint16_t amount = size - offset > sizeof(buffer) ? sizeof(buffer) : (uint16_t)(size - offset);
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) { readOk = false; break; }
    const int received = file.read(buffer, amount);
    SpiBusLock::unlock();
    if (received != amount || !body.concat(reinterpret_cast<const char *>(buffer), amount)) {
      readOk = false;
      break;
    }
    offset += amount;
  }
  if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    file.close();
    SpiBusLock::unlock();
  }
  if (!readOk) {
    browserLog(String(F("session page cache read failed: ")) + entry.path);
    if (removeSessionFileLocked(entry.path)) removeSessionPageLocked((uint8_t)index);
    body = "";
    return false;
  }
  entry.lastUse = nextSessionCacheUse();
  contentType = entry.contentType;
  return true;
}

bool BrowserScreen::storeSessionPage(const String &url, const String &contentType, const String &body) {
  if (body.length() > MaxBodyBytes) {
    browserLog(String(F("session page cache skip: body too large, bytes=")) + String(body.length()));
    return false;
  }
  HttpLock lock(_sessionCacheMutex);
  if (!lock.locked()) {
    browserLog(F("session page cache store failed: cache lock unavailable"));
    return false;
  }
  if (!initializeSessionCacheLocked()) {
    browserLog(F("session page cache store failed: cache initialization failed"));
    return false;
  }
  int8_t existing = sessionPageIndex(url);
  if (existing >= 0) {
    if (!removeSessionFileLocked(_sessionPages[existing].path)) return false;
    removeSessionPageLocked((uint8_t)existing);
  }
  // Browser documents are the Back cache; discard preview images before any
  // retained page body when making room for another document.
  if (!ensureSessionCacheSpaceLocked(body.length(), true)) {
    browserLog(F("session page cache store failed: no space after image eviction"));
    return false;
  }
  int8_t slot = -1;
  for (uint8_t i = 0; i < MaxSessionPages; i++) {
    if (!_sessionPages[i].used) { slot = i; break; }
  }
  if (slot < 0) {
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < MaxSessionPages; i++) {
      if (_sessionPages[i].lastUse < _sessionPages[oldest].lastUse) oldest = i;
    }
    if (!removeSessionFileLocked(_sessionPages[oldest].path)) return false;
    removeSessionPageLocked(oldest);
    slot = oldest;
  }
  const String path = nextSessionCachePath("page-", ".html");
  const String temporary = path + F(".tmp");
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  SD.remove(temporary.c_str());
  File file = SD.open(temporary.c_str(), FILE_WRITE);
  SpiBusLock::unlock();
  if (!file) {
    browserLog(String(F("session page cache store failed: cannot create ")) + temporary);
    return false;
  }
  uint32_t offset = 0;
  bool written = true;
  while (offset < body.length()) {
    const uint16_t amount = body.length() - offset > 512 ? 512 : (uint16_t)(body.length() - offset);
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) { written = false; break; }
    const size_t count = file.write(reinterpret_cast<const uint8_t *>(body.c_str()) + offset, amount);
    SpiBusLock::unlock();
    if (count != amount) { written = false; break; }
    offset += amount;
    yield();
  }
  if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    file.close();
    SpiBusLock::unlock();
  } else {
    written = false;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  // FAT rename does not replace an existing target. A cache directory can
  // contain an untracked file after power loss or interrupted cleanup, so
  // remove that stale target before committing this new atomic write.
  bool committed = false;
  if (written) {
    const bool targetReady = !SD.exists(path.c_str()) || SD.remove(path.c_str());
    committed = targetReady && SD.rename(temporary.c_str(), path.c_str());
  }
  if (!committed) SD.remove(temporary.c_str());
  SpiBusLock::unlock();
  if (!committed) {
    browserLog(String(F("session page cache store failed: cannot commit ")) + path);
    return false;
  }
  _sessionPages[slot] = {url, contentType, path, body.length(), nextSessionCacheUse(), true};
  _sessionCacheBytes += body.length();
  browserLog(String(F("page stored in session cache: ")) + url);
  return true;
}

String BrowserScreen::sessionImagePath(const String &url) {
  HttpLock lock(_sessionCacheMutex);
  if (!lock.locked() || !initializeSessionCacheLocked()) return "";
  const int8_t index = sessionImageIndex(url);
  if (index < 0) return "";
  SessionImageCacheEntry &entry = _sessionImages[index];
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return "";
  const bool exists = SD.exists(entry.path.c_str());
  SpiBusLock::unlock();
  if (!exists) {
    removeSessionImageLocked((uint8_t)index);
    return "";
  }
  entry.lastUse = nextSessionCacheUse();
  return entry.path;
}

void BrowserScreen::discardSessionImage(const String &url) {
  HttpLock lock(_sessionCacheMutex);
  if (!lock.locked() || !initializeSessionCacheLocked()) return;
  const int8_t index = sessionImageIndex(url);
  if (index < 0) return;
  if (removeSessionFileLocked(_sessionImages[index].path)) {
    removeSessionImageLocked((uint8_t)index);
  }
}

String BrowserScreen::adoptSessionImage(const String &url, const String &downloadedPath,
                                        FileFormats::Format format) {
  if (!FileFormats::isImage(format)) return "";
  HttpLock lock(_sessionCacheMutex);
  if (!lock.locked() || !initializeSessionCacheLocked()) return "";
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return "";
  File source = SD.open(downloadedPath.c_str(), FILE_READ);
  const uint32_t bytes = source ? (uint32_t)source.size() : 0;
  if (source) source.close();
  SpiBusLock::unlock();
  if (bytes == 0) return "";
  const int8_t existing = sessionImageIndex(url);
  if (existing >= 0) {
    if (!removeSessionFileLocked(_sessionImages[existing].path)) return "";
    removeSessionImageLocked((uint8_t)existing);
  }
  if (!ensureSessionCacheSpaceLocked(bytes, true)) return "";
  int8_t slot = -1;
  for (uint8_t i = 0; i < MaxSessionImages; i++) {
    if (!_sessionImages[i].used) { slot = i; break; }
  }
  if (slot < 0) {
    uint8_t oldest = 0;
    for (uint8_t i = 1; i < MaxSessionImages; i++) {
      if (_sessionImages[i].lastUse < _sessionImages[oldest].lastUse) oldest = i;
    }
    if (!removeSessionFileLocked(_sessionImages[oldest].path)) return "";
    removeSessionImageLocked(oldest);
    slot = oldest;
  }
  const String path = nextSessionCachePath("image-", FileFormats::extension(format));
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return "";
  const bool moved = !SD.exists(path.c_str()) && SD.rename(downloadedPath.c_str(), path.c_str());
  SpiBusLock::unlock();
  if (!moved) return "";
  _sessionImages[slot] = {url, path, bytes, nextSessionCacheUse(), true};
  _sessionCacheBytes += bytes;
  browserLog(String(F("image stored in session cache: ")) + url);
  return path;
}

bool BrowserScreen::openUrl(const String &value, bool addToHistory, bool addToRecent,
                            bool useSessionCache) {
  return openRequest({HttpMethod::Get, value, ""}, addToHistory, addToRecent, useSessionCache);
}

bool BrowserScreen::openRequest(const HttpRequest &initialRequest, bool addToHistory,
                                bool addToRecent, bool useSessionCache) {
  _menuOpen = false;
  const bool hadDocument = !_showStart && _currentUrl.length() > 0;
  const String previousUrl = _currentUrl;
  String url;
  String error;
  if (!normalizeUrl(initialRequest.url, url, error)) {
    browserLog(String(F("URL rejected: ")) + error);
    _showStart = false;
    _message = error;
    releaseEmbeddedImages();
    clearRenderedDocumentStorage();
    draw();
    return false;
  }
  if (initialRequest.method == HttpMethod::Post && initialRequest.body.length() > MaxFormPostBytes) {
    _showStart = false;
    _message = "Form data is too large";
    releaseEmbeddedImages();
    clearRenderedDocumentStorage();
    draw();
    return false;
  }
  HttpRequest request = initialRequest;
  request.url = url;
  browserLog(String(F("navigation requested: method=")) +
             (request.method == HttpMethod::Post ? F("POST") : F("GET")) + F(", url=") + url +
             (request.method == HttpMethod::Post ? String(F(", bodyBytes=")) + String(request.body.length()) : String()));
  if (!useSessionCache) _usingSessionPage = false;
  String finalUrl = url;
  String body;
  String contentType;
  const bool cacheHit = useSessionCache && request.method == HttpMethod::Get &&
                        loadSessionPage(url, body, contentType);
  if (cacheHit) {
    browserLog(String(F("session cache hit: ")) + url);
  } else if (!_wifi.isConnected()) {
    browserLog(F("navigation blocked: Wi-Fi is disconnected"));
    _showStart = false;
    _message = "Connect to Wi-Fi first";
    releaseEmbeddedImages();
    clearRenderedDocumentStorage();
    draw();
    return false;
  }

  // Record the page we are leaving before its replacement starts loading.
  // A cancelled (or failed) request still leaves that page reachable through
  // Back; delaying this until after fetch() succeeds made the footer Back
  // button disappear after cancelling a navigation.
  const bool addedPreviousPageToHistory = addToHistory && hadDocument && previousUrl != url;
  if (addedPreviousPageToHistory) {
    browserLog(F("saving current page to back history"));
    addHistory(previousUrl);
  }

  // Invalidates queued image jobs before a new page takes the shared HTTP
  // client. A task already in a network read is allowed to finish safely;
  // its result is discarded.
  releaseEmbeddedImages();
  // The visual rows, links, and CSS rules contain Strings from the previous
  // document. Clearing their allocations before a new TLS handshake avoids
  // carrying an entire old page in scarce internal RAM while mbedTLS creates
  // its bignum working buffers.
  clearRenderedDocumentStorage();
  browserLog(String(F("previous document released: freeHeap=")) + String(ESP.getFreeHeap()) +
             F(", largestHeapBlock=") + String(ESP.getMaxAllocHeap()));

  _currentUrl = url;
  _showStart = false;
  _message = "";
  _title = "";
  _page = 0;
  _loadCancelled = false;
  _usingSessionPage = cacheHit;
  _deferCachedPageDraw = false;
  _deferCachedPageDrawStartedMs = 0;
  // Navigation can be initiated by the Back button itself.  That tap remains
  // down while this synchronous request starts, so treat it as already
  // consumed; otherwise pollLoadingCancel() cancels the Back request at once.
  _loadCancelTouchDown = _lastTouchDown;
  beginLoading(cacheHit ? F("Restoring cached page") : F("Preparing request"));
  draw();
  yield();

  String downloadedPath;
  if (!cacheHit && !fetch(request, finalUrl, body, contentType, downloadedPath, error)) {
    browserLog(String(F("navigation failed: url=")) + url + F(", phase=") + _loadingPhase +
               F(", error=") + error + F(", userCancelled=") +
               (_loadCancelled ? F("yes") : F("no")) + F(", activeImageGeneration=") +
               String(_activeImageGeneration) + F(", currentImageGeneration=") + String(_imageGeneration));
    finishLoading();
    _message = _loadCancelled ? "Loading cancelled" : error;
    draw();
    return false;
  }
  // Redirect Location headers are allowed to omit a trailing slash or use a
  // differently-cased host. Back always normalizes its target, so use the
  // same canonical form for the rendered page and its session-cache key.
  // Without this, a cached `https://example.com` was looked up as
  // `https://example.com/` and unnecessarily reloaded from the network.
  String canonicalFinalUrl;
  String canonicalFinalUrlError;
  if (normalizeUrl(finalUrl, canonicalFinalUrl, canonicalFinalUrlError)) {
    finalUrl = canonicalFinalUrl;
  } else {
    browserLog(String(F("final URL could not be normalized: ")) + canonicalFinalUrlError);
  }
  _currentUrl = finalUrl;
  setLoadingPhase(cacheHit ? F("Rendering cached page")
                            : (downloadedPath.length() ? F("Opening downloaded file") : F("Rendering page")));
  if (downloadedPath.length()) {
    FileFormats::Format format = FileFormats::fromPath(downloadedPath);
    FileFormats::Group group = FileFormats::group(format);

    // The download target is an action, not a new browser document.  Undo
    // the provisional history entry and restore this source document after
    // its viewer/player closes.  Rendering a replacement page here used to
    // leave users at the "Downloaded image opened in viewer" placeholder.
    const String returnUrl = hadDocument ? previousUrl : String();
    if (addedPreviousPageToHistory && _historyCount > 0 &&
        _historyUrls[_historyCount - 1] == previousUrl) {
      _historyCount--;
    }
    _downloadReturnUrl = returnUrl;
    if (addToRecent) addRecent(_currentUrl, _title);
    saveState();
    finishLoading();
    browserLog(String(F("download complete: ")) + downloadedPath);
    if (group == FileFormats::Group::Image &&
        _navigator.push(ScreenId::ImageViewer, ScreenData::imagePath(downloadedPath))) return true;
    if (group == FileFormats::Group::Audio &&
        _navigator.push(ScreenId::AudioPlayer, ScreenData::audioPath(downloadedPath))) return true;
    if (group == FileFormats::Group::Video &&
        _navigator.push(ScreenId::VideoPlayer, ScreenData::videoPath(downloadedPath))) return true;
    _downloadReturnUrl = "";
    _message = "Could not open downloaded file";
    draw();
    return true;
  }
  const bool htmlPage = contentType.indexOf("text/html") >= 0 ||
                        contentType.indexOf("application/xhtml") >= 0 || contentType.length() == 0;
  const bool textPage = contentType.indexOf("text/plain") >= 0;
  // Session pages are stored after normalization, so re-decoding a cached
  // Windows-1251/KOI8 response would corrupt its UTF-8 body.
  if (!cacheHit && (htmlPage || textPage)) {
    browserLog(F("normalizing text response"));
    String encodingError;
    String normalizedBody;
    if (!BrowserText::normalizeToUtf8(body, contentType, normalizedBody, encodingError)) {
      finishLoading();
      renderPlainText("(" + encodingError + ")", "Unsupported text encoding");
      _message = "";
      browserLog(String(F("text decoding failed: ")) + encodingError);
      draw();
      return false;
    }
    body = normalizedBody;
    browserLog(String(F("text normalized: bytes=")) + String(body.length()));
  }
  if (!cacheHit && (htmlPage || textPage)) {
    browserLog(F("storing session page"));
    if (storeSessionPage(_currentUrl, contentType, body)) {
      browserLog(F("session page stored"));
    } else {
      browserLog(F("session page was not cached"));
    }
  }
  if (htmlPage) {
    renderDocument(body);
  } else if (textPage) {
    renderPlainText(body, "Text page");
  } else {
    renderPlainText("[Unsupported content: " + (contentType.length() ? contentType : String("unknown type")) + "]",
                    "Unsupported content");
  }
  if (addToRecent) addRecent(_currentUrl, _title);
  saveState();
  // Draw the cached document immediately.  Image jobs run afterward: their
  // source is read from the session cache when available, and only a missing
  // image is fetched on the worker without delaying the restored page.
  finishLoading();
  browserLog(String(F("page ready: title=\"")) + _title + F("\", rows=") +
             String(_document.rowCount()) + F(", pages=") + String(_document.pageCount()) +
             F(", images=") + String(_document.imageCount()));
  draw();
  return true;
}

void BrowserScreen::goBack() {
  if (_historyCount == 0) {
    browserLog(F("back history empty; showing start page"));
    releaseEmbeddedImages();
    _showStart = true;
    _page = 0;
    _message = "";
    draw();
    return;
  }
  const String target = _historyUrls[_historyCount - 1];
  browserLog(String(F("loading previous page: ")) + target);
  if (openUrl(target, false, false, true)) {
    _historyCount--;
    browserLog(F("back navigation complete"));
  }
}

bool BrowserScreen::normalizeUrl(const String &value, String &normalized, String &error) const {
  return BrowserTransport::normalizeUrl(value, normalized, error);
}

bool BrowserScreen::parseUrl(const String &value, Url &url) const {
  return BrowserTransport::parseUrl(value, url);
}

bool BrowserScreen::fetch(const HttpRequest &initialRequest, String &finalUrl, String &body,
                          String &contentType, String &downloadedPath, String &error,
                          uint32_t downloadLimit) {
  setLoadingPhase(F("Waiting for network access"));
  const bool calledByImageWorker = _imageWorker && xTaskGetCurrentTaskHandle() == _imageWorker;
  const uint32_t waitStartedMs = millis();
  // releaseEmbeddedImages() invalidates a worker job immediately, but a
  // decoder already running on the other core still owns its temporary heap
  // until it returns. TLS needs several internal-RAM allocations during its
  // handshake, so do not overlap it with that cleanup work.
  if (!calledByImageWorker) {
    setLoadingPhase(F("Waiting for image cleanup"));
    while (_activeImageGeneration) {
      if (pollLoadingCancel()) {
        error = "Loading cancelled";
        browserLog(String(F("image cleanup wait cancelled: waitedMs=")) + String(millis() - waitStartedMs));
        return false;
      }
      if (millis() - waitStartedMs >= NetworkLockTimeoutMs) {
        browserLog(String(F("image cleanup wait timed out: waitedMs=")) + String(millis() - waitStartedMs) +
                   F(", activeImageGeneration=") + String(_activeImageGeneration));
        error = "Browser is busy; try again";
        return false;
      }
      delay(1);
      yield();
    }
  }
  // Inline images are fetched on the second core but share the two WiFiClient
  // instances with foreground navigation.  Do not block the Arduino/UI task
  // indefinitely if a remote image server has stalled.  Poll in short slices
  // so the footer cancel button remains functional while it waits.
  HttpLock lock(_transport.mutex(), 0);
  while (!lock.acquire(pdMS_TO_TICKS(50))) {
    if (pollLoadingCancel()) {
      error = "Loading cancelled";
      browserLog(String(F("network access wait cancelled: waitedMs=")) +
                 String(millis() - waitStartedMs));
      return false;
    }
    if (millis() - waitStartedMs >= NetworkLockTimeoutMs) {
      browserLog(String(F("network access wait timed out: waitedMs=")) +
                 String(millis() - waitStartedMs) + F(", activeImageGeneration=") +
                 String(_activeImageGeneration) + F(", currentImageGeneration=") + String(_imageGeneration));
      error = "Browser is busy; try again";
      return false;
    }
    delay(1);
    yield();
  }
  if (!lock.locked()) {
    browserLog(F("HTTP lock unavailable"));
    error = "Browser is busy";
    return false;
  }
  browserLog(F("network access acquired"));
  body = "";
  contentType = "";
  downloadedPath = "";
  HttpRequest request = initialRequest;
  for (uint8_t redirect = 0; redirect <= MaxRedirects; redirect++) {
    Url url;
    if (!parseUrl(request.url, url)) {
      browserLog(F("invalid redirect target"));
      error = "Invalid redirect URL";
      return false;
    }
    browserLog(String(F("HTTP request ")) + String(redirect + 1) + F("/") + String(MaxRedirects + 1) +
               F(": method=") + (request.method == HttpMethod::Post ? F("POST") : F("GET")) +
               F(", url=") + request.url +
               (request.method == HttpMethod::Post ? String(F(", bodyBytes=")) + String(request.body.length()) : String()));
    HttpResponse response{};
    WiFiClient *client = nullptr;
    if (!requestOne(url, request.method, request.body, response, client, error)) {
      browserLog(String(F("HTTP request failed: url=")) + request.url + F(", phase=") + _loadingPhase +
                 F(", error=") + error + F(", userCancelled=") +
                 (_loadCancelled ? F("yes") : F("no")));
      return false;
    }
    if (response.statusCode >= 300 && response.statusCode <= 399) {
      client->stop();
      if (response.location.length() == 0) {
        browserLog(String(F("redirect rejected: url=")) + request.url + F(", status=") + String(response.statusCode) +
                   F(", reason=no Location header"));
        error = "Redirect has no destination";
        return false;
      }
      const String resolvedLocation = resolveUrl(url, response.location);
      browserLog(String(F("redirect received: status=")) + String(response.statusCode) +
                 F(", request=") + request.url + F(", Location=") + response.location +
                 F(", resolved=") + resolvedLocation +
                 F(", returnsToInitial=") +
                 (resolvedLocation == initialRequest.url ? F("yes") : F("no")));
      if (response.statusCode == 301 || response.statusCode == 302 || response.statusCode == 303) {
        request.method = HttpMethod::Get;
        request.body = "";
      }
      request.url = resolvedLocation;
      continue;
    }
    if (response.statusCode == 401) {
      client->stop();
      browserLog(String(F("server requires unsupported Basic authentication: url=")) + request.url);
      error = "Basic authentication is unsupported";
      return false;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
      client->stop();
      browserLog(String(F("HTTP request rejected: url=")) + request.url + F(", status=") + String(response.statusCode));
      error = "HTTP error " + String(response.statusCode);
      return false;
    }
    contentType = response.contentType;
    FileFormats::Format format = downloadableFormatForResponse(contentType, url);
    bool downloadable = FileFormats::isDownloadable(format);
    browserLog(String(F("response accepted: type=")) +
               (contentType.length() ? contentType : String(F("unspecified"))) +
               (response.contentDisposition.length() ? String(F(", disposition=")) + response.contentDisposition
                                                     : String()) +
               F(", length=") + String(response.contentLength) +
               (response.chunked ? F(", chunked") : F("")) +
               (downloadable ? F(", media download") : F(", page response")));
    bool readOk = false;
    if (downloadable && response.compressed) {
      browserLog(String(F("compressed media download rejected: url=")) + request.url + F(", encoding is not identity"));
      error = "Compressed file response unsupported";
    } else if (downloadable) {
      readOk = readDownloadBody(*client, response.chunked, response.contentLength, format,
                                downloadedPath, error, downloadLimit,
                                downloadFileName(response.contentDisposition, url, format));
    } else {
      readOk = readBody(*client, response.chunked, response.contentLength, body, error);
    }
    if (!readOk) {
      browserLog(String(F("response transfer failed: url=")) + request.url + F(", phase=") + _loadingPhase +
                 F(", error=") + error + F(", retainedBytes=") + String(body.length()) + F(", ") +
                 clientState(*client));
      client->stop();
      return false;
    }
    client->stop();
    finalUrl = request.url;
    browserLog(String(F("HTTP transfer complete: ")) + finalUrl);
    return true;
  }
  browserLog(String(F("redirect limit reached: initialUrl=")) + initialRequest.url + F(", limit=") + String(MaxRedirects));
  error = "Too many redirects";
  return false;
}

bool BrowserScreen::requestOne(const Url &url, HttpMethod method, const String &requestBody,
                               HttpResponse &response, WiFiClient *&client, String &error) {
  if (pollLoadingCancel()) {
    error = "Loading cancelled";
    browserLog(String(F("request cancelled before connect: url=")) + url.scheme + F("://") + url.host + url.path);
    return false;
  }
  _transport.stopClients();
  client = &_transport.plainClient();
  if (url.scheme == "https") {
    _transport.secureClient().setInsecure();
    _transport.secureClient().setHandshakeTimeout(ConnectTimeoutMs / 1000);
    client = &_transport.secureClient();
  }
  setLoadingPhase(String(F("Connecting to ")) + url.host);
  browserLog(String(F("connecting: ")) + url.scheme + F("://") + url.host + F(":") + String(url.port));
  IPAddress resolvedAddress;
  // The ESP32 Arduino core exposes hostByName without a caller-supplied
  // timeout, so keep this diagnostic compatible with both older and newer
  // core releases.
  const int dnsResult = WiFi.hostByName(url.host.c_str(), resolvedAddress);
  if (dnsResult != 1) {
    browserLog(String(F("DNS lookup failed: host=")) + url.host + F(", result=") + String(dnsResult) +
               F(", wifiStatus=") + String((int)WiFi.status()) + F(", rssi=") + String(WiFi.RSSI()));
    error = "DNS lookup failed";
    return false;
  }
  browserLog(String(F("DNS resolved: host=")) + url.host + F(", address=") + resolvedAddress.toString());
  browserLog(String(F("TLS memory before connect: freeHeap=")) + String(ESP.getFreeHeap()) +
             F(", largestHeapBlock=") + String(ESP.getMaxAllocHeap()) + F(", freePsram=") +
             String(ESP.getFreePsram()) + F(", largestPsramBlock=") + String(ESP.getMaxAllocPsram()));
  // connect() can include DNS lookup and the TLS handshake.  Supplying the
  // same deadline used by response reads prevents a black-hole host from
  // making the browser appear frozen before its cancel polling can begin.
  const uint32_t initialConnectStartedMs = millis();
  bool connected = client->connect(url.host.c_str(), url.port, (int32_t)ConnectTimeoutMs);
  const uint32_t initialConnectElapsedMs = millis() - initialConnectStartedMs;
  String rawTcpState = "not-tested";
  uint32_t retryConnectElapsedMs = 0;
  if (!connected && url.scheme == "https") {
    // A generic -1 from NetworkClientSecure can be either a socket failure
    // or a handshake timeout. Test TCP once first; retry TLS only when the
    // server is demonstrably reachable, avoiding a second long wait for an
    // unreachable address.
    _transport.plainClient().stop();
    rawTcpState = _transport.plainClient().connect(resolvedAddress, url.port, 1000) ? "connected" : "failed";
    _transport.plainClient().stop();
    if (rawTcpState == "connected") {
      _transport.secureClient().stop();
      _transport.secureClient().setHandshakeTimeout(TlsRetryHandshakeTimeoutSeconds);
      browserLog(String(F("HTTPS retrying resolved address: host=")) + url.host + F(", address=") +
                 resolvedAddress.toString() + F(", handshakeTimeoutSeconds=") +
                 String(TlsRetryHandshakeTimeoutSeconds));
      const uint32_t retryConnectStartedMs = millis();
      connected = _transport.secureClient().connect(resolvedAddress, url.port, url.host.c_str(), nullptr, nullptr, nullptr);
      retryConnectElapsedMs = millis() - retryConnectStartedMs;
      if (connected) browserLog(String(F("HTTPS retry via resolved address succeeded: elapsedMs=")) +
                                String(retryConnectElapsedMs));
    }
  }
  if (!connected) {
    char tlsErrorText[96] = {};
    const int tlsError = url.scheme == "https"
      ? _transport.secureClient().lastError(tlsErrorText, sizeof(tlsErrorText)) : 0;
    browserLog(String(F("connection failed: host=")) + url.host + F(", port=") + String(url.port) +
               F(", initialConnectMs=") + String(initialConnectElapsedMs) + F(", timeoutMs=") +
               String(ConnectTimeoutMs) + F(", wifiStatus=") +
               String((int)WiFi.status()) + F(", rssi=") + String(WiFi.RSSI()) + F(", tlsError=") +
               String(tlsError) + F(", tlsDetail=") + tlsErrorText + F(", retryConnectMs=") +
               String(retryConnectElapsedMs) + F(", rawTcp=") +
               rawTcpState + F(", ") + clientState(*client));
    error = _loadCancelled ? "Loading cancelled" : "Connection failed";
    return false;
  }
  setLoadingPhase(F("Sending request"));
  if (pollLoadingCancel()) {
    client->stop();
    error = "Loading cancelled";
    browserLog(String(F("request cancelled after connect: host=")) + url.host + F(", ") + clientState(*client));
    return false;
  }
  String hostHeader = url.host;
  bool defaultPort = (url.scheme == "http" && url.port == 80) || (url.scheme == "https" && url.port == 443);
  if (!defaultPort) hostHeader += ":" + String(url.port);
  const bool post = method == HttpMethod::Post;
  const char *userAgent = _androidUserAgent ? MobileUserAgent : EspUserAgent;
  String request = String(post ? "POST" : "GET") + " " + url.path + " HTTP/1.1\r\nHost: " + hostHeader +
                   "\r\nUser-Agent: " + userAgent + "\r\nAccept: text/html,*/*;q=0.1\r\n"
                   "Accept-Encoding: identity\r\nConnection: close\r\n";
  if (post) {
    request += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
               String(requestBody.length()) + "\r\n";
  }
  String cookies = cookieHeaderFor(url);
  if (cookies.length()) request += "Cookie: " + cookies + "\r\n";
  request += "\r\n";
  const size_t headerBytes = client->print(request);
  if (headerBytes != request.length()) {
    browserLog(String(F("request header write failed: host=")) + url.host + F(", sent=") + String(headerBytes) +
               F(", expected=") + String(request.length()) + F(", ") + clientState(*client));
    client->stop();
    error = "Request failed";
    return false;
  }
  if (post && requestBody.length()) {
    if (pollLoadingCancel()) {
      client->stop();
      error = "Loading cancelled";
      return false;
    }
    const size_t bodyBytes = client->write(reinterpret_cast<const uint8_t *>(requestBody.c_str()), requestBody.length());
    if (bodyBytes != requestBody.length()) {
      browserLog(String(F("request body write failed: host=")) + url.host + F(", sent=") + String(bodyBytes) +
                 F(", expected=") + String(requestBody.length()) + F(", ") + clientState(*client));
      client->stop();
      error = "Request failed";
      return false;
    }
  }
  browserLog(String(F("request sent: method=")) + (post ? F("POST") : F("GET")) + F(", host=") + url.host +
             F(", bodyBytes=") + String(post ? requestBody.length() : 0));

  String line;
  setLoadingPhase(F("Waiting for response"));
  if (!readLine(*client, line, ConnectTimeoutMs) || !startsWithIgnoreCase(line, "HTTP/")) {
    browserLog(String(F("invalid or timed out HTTP status line: host=")) + url.host + F(", bytes=") +
               String(line.length()) + F(", timeoutMs=") + String(ConnectTimeoutMs) + F(", ") + clientState(*client));
    client->stop();
    error = "Invalid HTTP response";
    return false;
  }
  int firstSpace = line.indexOf(' ');
  response.statusCode = firstSpace >= 0 ? line.substring(firstSpace + 1).toInt() : 0;
  if (response.statusCode == 0) {
    browserLog(String(F("invalid HTTP status: host=")) + url.host + F(", line=") + line);
    client->stop();
    client = nullptr;
    error = "Invalid HTTP status";
    return false;
  }
  browserLog(String(F("HTTP status: ")) + String(response.statusCode));

  response.chunked = false;
  response.compressed = false;
  response.contentLength = -1;
  response.contentType = "";
  response.contentDisposition = "";
  response.location = "";
  bool headersComplete = false;
  for (uint8_t count = 0; count < MaxResponseHeaderLines; count++) {
    bool lineTruncated = false;
    if (!readLine(*client, line, ConnectTimeoutMs, true, &lineTruncated)) {
      browserLog(String(F("response header read failed: host=")) + url.host + F(", headerCount=") +
                 String(count) + F(", retainedLineBytes=") + String(line.length()) + F(", timeoutMs=") +
                 String(ConnectTimeoutMs) + F(", ") + clientState(*client));
      client->stop();
      error = "Response headers timed out";
      return false;
    }
    if (lineTruncated) {
      browserLog(String(F("discarded oversized response header: host=")) + url.host + F(", headerCount=") +
                 String(count) + F(", retainedBytes=") + String(line.length()) + F(", maxBytes=") +
                 String(MaxHttpLineBytes));
      continue;
    }
    if (line.length() == 0) {
      headersComplete = true;
      break;
    }
    int colon = line.indexOf(':');
    if (colon <= 0) continue;
    String name = lower(trimCopy(line.substring(0, colon)));
    String headerValue = trimCopy(line.substring(colon + 1));
    if (name == "content-length") response.contentLength = headerValue.toInt();
    else if (name == "transfer-encoding" && lower(headerValue).indexOf("chunked") >= 0) response.chunked = true;
    else if (name == "content-encoding" && !headerValue.equalsIgnoreCase("identity")) response.compressed = true;
    else if (name == "content-type") response.contentType = lower(headerValue);
    else if (name == "content-disposition") response.contentDisposition = headerValue;
    else if (name == "location") response.location = headerValue;
    else if (name == "set-cookie") acceptCookie(headerValue, url);
  }
  if (!headersComplete) {
    browserLog(String(F("too many response headers: host=")) + url.host + F(", limit=") +
               String(MaxResponseHeaderLines) + F(", ") + clientState(*client));
    client->stop();
    error = "Too many response headers";
    return false;
  }
  browserLog(String(F("headers complete: length=")) + String(response.contentLength) +
             F(", type=") + (response.contentType.length() ? response.contentType : String(F("unspecified"))) +
             (response.contentDisposition.length() ? String(F(", disposition=")) + response.contentDisposition
                                                   : String()) +
             (response.chunked ? F(", chunked") : F("")) +
             (response.compressed ? F(", compressed") : F("")));
  return true;
}

bool BrowserScreen::readLine(WiFiClient &client, String &line, uint32_t timeoutMs,
                             bool discardLongLine, bool *lineTruncated) {
  line = "";
  if (lineTruncated) *lineTruncated = false;
  bool truncated = false;
  uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (pollLoadingCancel()) return false;
    if (_activeImageGeneration && _activeImageGeneration != _imageGeneration) return false;
    while (client.available()) {
      int value = client.read();
      if (value < 0) continue;
      if (value == '\n') {
        if (!truncated && line.endsWith("\r")) line.remove(line.length() - 1);
        if (lineTruncated) *lineTruncated = truncated;
        return true;
      }
      if (truncated) continue;
      if (line.length() >= MaxHttpLineBytes) {
        if (!discardLongLine) return false;
        truncated = true;
        continue;
      }
      line += (char)value;
    }
    if (!client.connected() && !client.available()) {
      if (lineTruncated) *lineTruncated = truncated;
      return line.length() > 0;
    }
    delay(1);
    yield();
  }
  return false;
}

int BrowserScreen::readByte(WiFiClient &client, uint32_t timeoutMs) {
  uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (pollLoadingCancel()) return -1;
    if (_activeImageGeneration && _activeImageGeneration != _imageGeneration) return -1;
    if (client.available()) return client.read();
    if (!client.connected()) return -1;
    delay(1);
    yield();
  }
  return -1;
}

bool BrowserScreen::readBody(WiFiClient &client, bool chunked, int32_t contentLength,
                             String &body, String &error) {
  setLoadingPhase(F("Loading page"), chunked ? -1 : contentLength);
  browserLog(String(F("reading page body")) + (chunked ? F(" (chunked)") : F("")) +
             F(", expected bytes=") + String(contentLength));
  body = "";
  uint32_t reserveBytes = contentLength > 0
    ? (contentLength < (int32_t)MaxBodyBytes ? (uint32_t)contentLength : MaxBodyBytes)
    : 4096;
  if (!body.reserve(reserveBytes)) {
    browserLog(String(F("page body allocation failed: requestedBytes=")) + String(reserveBytes) +
               F(", freeHeap=") + String(ESP.getFreeHeap()) + F(", largestBlock=") +
               String(ESP.getMaxAllocHeap()));
    error = "Not enough memory for page";
    return false;
  }
  uint32_t total = 0;
  uint32_t reported = 0;
  auto reportProgress = [&]() {
    if (total - reported >= 1024) {
      advanceLoading(total - reported);
      reported = total;
    }
  };
  auto finishProgress = [&]() {
    if (total > reported) advanceLoading(total - reported);
  };
  if (chunked) {
    while (true) {
      String sizeLine;
      if (!readLine(client, sizeLine, ConnectTimeoutMs)) {
        error = "Page body timed out";
        return false;
      }
      int semicolon = sizeLine.indexOf(';');
      if (semicolon >= 0) sizeLine.remove(semicolon);
      uint32_t chunk = strtoul(sizeLine.c_str(), nullptr, 16);
      if (chunk == 0) {
        do {
          if (!readLine(client, sizeLine, ConnectTimeoutMs)) break;
        } while (sizeLine.length());
        finishProgress();
        browserLog(String(F("page body complete: bytes=")) + String(total));
        return true;
      }
      // Do not reject a large page. Retain the leading bounded portion and
      // close the connection; old pages commonly put useful body text well
      // inside this limit.
      if (chunk > MaxBodyBytes - total) {
        uint32_t keep = MaxBodyBytes - total;
        for (uint32_t i = 0; i < keep; i++) {
          int value = readByte(client, ConnectTimeoutMs);
          if (value < 0) {
            error = "Page body timed out";
            return false;
          }
          body += (char)value;
          total++;
          reportProgress();
        }
        finishProgress();
        browserLog(String(F("page body reached retained limit: bytes=")) + String(total));
        return true;
      }
      for (uint32_t i = 0; i < chunk; i++) {
        int value = readByte(client, ConnectTimeoutMs);
        if (value < 0) {
          error = "Page body timed out";
          return false;
        }
        body += (char)value;
        total++;
        reportProgress();
      }
      int cr = readByte(client, ConnectTimeoutMs);
      int lf = readByte(client, ConnectTimeoutMs);
      if (cr != '\r' || lf != '\n') {
        error = "Invalid chunked response";
        return false;
      }
    }
  }

  while (contentLength < 0 || total < (uint32_t)contentLength) {
    int value = readByte(client, ConnectTimeoutMs);
    if (value < 0) {
      if (contentLength < 0) {
        finishProgress();
        browserLog(String(F("page body complete at connection close: bytes=")) + String(total));
        return true;
      }
      error = "Incomplete page body";
      return false;
    }
    if (total >= MaxBodyBytes) {
      finishProgress();
      browserLog(String(F("page body reached retained limit: bytes=")) + String(total));
      return true;
    }
    body += (char)value;
    total++;
    reportProgress();
  }
  finishProgress();
  browserLog(String(F("page body complete: bytes=")) + String(total));
  return true;
}

FileFormats::Format BrowserScreen::downloadableFormatForResponse(const String &contentType, const Url &url) const {
  String type = lower(trimCopy(contentType));
  int semicolon = type.indexOf(';');
  if (semicolon >= 0) type.remove(semicolon);
  type.trim();
  FileFormats::Format format = FileFormats::fromContentType(type);
  if (FileFormats::isDownloadable(format)) return format;
  if (type.length() == 0 || type == "application/octet-stream") {
    format = FileFormats::fromPath(url.path);
    if (FileFormats::isDownloadable(format)) return format;
  }
  return FileFormats::Format::Unknown;
}

String BrowserScreen::downloadFileName(const String &contentDisposition, const Url &url,
                                       FileFormats::Format format) const {
  String candidate;
  String disposition = contentDisposition;
  const String lowerDisposition = lower(disposition);
  int position = lowerDisposition.indexOf("filename*=");
  if (position >= 0) {
    candidate = trimCopy(disposition.substring(position + 10));
    const int separator = candidate.indexOf(';');
    if (separator >= 0) candidate.remove(separator);
    if (candidate.length() >= 2 && candidate[0] == '"' && candidate[candidate.length() - 1] == '"') {
      candidate = candidate.substring(1, candidate.length() - 1);
    }
    const int charsetEnd = candidate.indexOf('\'');
    const int languageEnd = charsetEnd < 0 ? -1 : candidate.indexOf('\'', charsetEnd + 1);
    if (languageEnd >= 0) candidate = candidate.substring(languageEnd + 1);
  }
  if (!candidate.length()) {
    position = lowerDisposition.indexOf("filename=");
    if (position >= 0) {
      candidate = trimCopy(disposition.substring(position + 9));
      const int separator = candidate.indexOf(';');
      if (separator >= 0) candidate.remove(separator);
      if (candidate.length() >= 2 && candidate[0] == '"' && candidate[candidate.length() - 1] == '"') {
        candidate = candidate.substring(1, candidate.length() - 1);
      }
    }
  }
  if (!candidate.length()) {
    candidate = url.path;
    const int query = candidate.indexOf('?');
    if (query >= 0) candidate.remove(query);
    const int slash = candidate.lastIndexOf('/');
    if (slash >= 0) candidate = candidate.substring(slash + 1);
  }

  String decoded;
  for (uint16_t index = 0; index < candidate.length(); index++) {
    if (candidate[index] == '%' && index + 2 < candidate.length() &&
        isxdigit((unsigned char)candidate[index + 1]) && isxdigit((unsigned char)candidate[index + 2])) {
      char encoded[] = {candidate[index + 1], candidate[index + 2], '\0'};
      decoded += (char)strtoul(encoded, nullptr, 16);
      index += 2;
    } else {
      decoded += candidate[index];
    }
  }

  String safe;
  for (uint16_t index = 0; index < decoded.length() && safe.length() < 96; index++) {
    const uint8_t character = (uint8_t)decoded[index];
    if (character < 0x20 || character == 0x7f || character == '/' || character == '\\' ||
        character == ':' || character == '*' || character == '?' || character == '"' ||
        character == '<' || character == '>' || character == '|') continue;
    safe += (char)character;
  }
  safe.trim();
  while (safe.endsWith(".")) safe.remove(safe.length() - 1);
  if (!safe.length() || safe == "." || safe == "..") return "";

  if (FileFormats::fromPath(safe) != format) {
    const int dot = safe.lastIndexOf('.');
    if (dot > 0) safe.remove(dot);
    safe += FileFormats::extension(format);
  }
  return safe;
}

bool BrowserScreen::isSupportedDownloadFile(const String &path, FileFormats::Format expected) const {
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  File file = SD.open(path.c_str(), FILE_READ);
  uint8_t signature[32] = {};
  const int bytes = file ? file.read(signature, sizeof(signature)) : 0;
  if (file) file.close();
  SpiBusLock::unlock();
  FileFormats::Format actual = FileFormats::fromSignature(signature, bytes > 0 ? (size_t)bytes : 0);
  if (actual == expected) return true;
  // Some legitimate 3GP writers use a generic ISO-BMFF major brand. Permit
  // that one ambiguity for a declared .3gp response, but do not accept a
  // positively identified 3GP file when the response claimed MP4.
  return expected == FileFormats::Format::ThreeGp && actual == FileFormats::Format::Mp4;
}

bool BrowserScreen::readDownloadBody(WiFiClient &client, bool chunked, int32_t contentLength,
                                     FileFormats::Format format, String &downloadedPath, String &error,
                                     uint32_t downloadLimit, const String &suggestedFileName) {
  downloadedPath = "";
  setLoadingPhase(String(F("Downloading ")) + FileFormats::extension(format), chunked ? -1 : contentLength);
  browserLog(String(F("starting file download: format=")) + FileFormats::extension(format) +
             F(", expected bytes=") + String(contentLength) +
             (chunked ? F(", chunked") : F("")));
  if (!_sdCard.isMounted()) {
    browserLog(F("download blocked: SD card is not mounted"));
    error = "SD card required for downloads";
    return false;
  }
  if (downloadLimit == 0 || downloadLimit > MaxDownloadBytes) downloadLimit = MaxDownloadBytes;
  if (contentLength > (int32_t)downloadLimit) {
    browserLog(F("download rejected: declared size exceeds limit"));
    error = "File exceeds download limit";
    return false;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    browserLog(F("download blocked: SD card bus busy"));
    error = "SD card is busy";
    return false;
  }
  // Downloads are user files, not disposable browser cache. Create the
  // top-level directory on first use so it is also visible in Files.
  bool ready = SD.exists(DownloadDirectory) || SD.mkdir(DownloadDirectory);
  if (!ready) {
    SpiBusLock::unlock();
    error = "Cannot create Downloads directory";
    browserLog(F("Downloads directory creation failed"));
    return false;
  }

  const String extension = FileFormats::extension(format);
  const uint32_t stamp = millis();
  String path;
  String tempPath;
  File file;
  for (uint16_t attempt = 0; attempt < 1000; attempt++) {
    String name = suggestedFileName;
    if (!name.length()) name = "media-" + String((unsigned long)stamp) + extension;
    if (attempt) {
      const int dot = name.lastIndexOf('.');
      const String suffix = dot > 0 ? name.substring(dot) : extension;
      const String stem = dot > 0 ? name.substring(0, dot) : name;
      name = stem + "-" + String(attempt) + suffix;
    }
    path = String(DownloadDirectory) + "/" + name;
    tempPath = path + ".tmp";
    if (SD.exists(path)) continue;
    if (SD.exists(tempPath)) SD.remove(tempPath);
    file = SD.open(tempPath.c_str(), FILE_WRITE);
    if (file) break;
  }
  SpiBusLock::unlock();
  if (!file) {
    browserLog(F("download file creation failed"));
    error = "Cannot create download file";
    return false;
  }

  auto removeTemp = [&]() {
    // A failed transfer must not leave a partial file behind. Waiting here is
    // safe because every normal SD user releases the shared bus lock after
    // each individual operation.
    SpiBusLock::lock();
    if (file) file.close();
    if (tempPath.length()) SD.remove(tempPath.c_str());
    SpiBusLock::unlock();
  };
  uint32_t total = 0;
  auto writeBytes = [&](const uint8_t *bytes, uint16_t length) -> bool {
    if (length > downloadLimit - total) {
      error = "File exceeds download limit";
      return false;
    }
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
      error = "SD card is busy";
      return false;
    }
    size_t written = file.write(bytes, length);
    SpiBusLock::unlock();
    if (written != length) {
      error = "File download write failed";
      return false;
    }
    total += length;
    advanceLoading(length);
    yield();
    return true;
  };
  auto receiveBytes = [&](uint32_t length) -> bool {
    uint8_t buffer[512];
    while (length) {
      uint16_t amount = length > sizeof(buffer) ? sizeof(buffer) : (uint16_t)length;
      for (uint16_t i = 0; i < amount; i++) {
        int value = readByte(client, ConnectTimeoutMs);
        if (value < 0) {
          error = "File download timed out";
          return false;
        }
        buffer[i] = (uint8_t)value;
      }
      if (!writeBytes(buffer, amount)) return false;
      length -= amount;
    }
    return true;
  };

  bool complete = true;
  if (chunked) {
    while (complete) {
      String sizeLine;
      if (!readLine(client, sizeLine, ConnectTimeoutMs)) {
        error = "File download timed out";
        complete = false;
        break;
      }
      int semicolon = sizeLine.indexOf(';');
      if (semicolon >= 0) sizeLine.remove(semicolon);
      char *end = nullptr;
      uint32_t amount = strtoul(sizeLine.c_str(), &end, 16);
      if (sizeLine.length() == 0 || !end || *end != '\0') {
        error = "Invalid chunked download";
        complete = false;
        break;
      }
      if (amount == 0) {
        do {
          if (!readLine(client, sizeLine, ConnectTimeoutMs)) {
            error = "File download timed out";
            complete = false;
            break;
          }
        } while (sizeLine.length());
        break;
      }
      if (amount > downloadLimit - total || !receiveBytes(amount)) {
        if (error.length() == 0) error = "File exceeds download limit";
        complete = false;
        break;
      }
      int cr = readByte(client, ConnectTimeoutMs);
      int lf = readByte(client, ConnectTimeoutMs);
      if (cr != '\r' || lf != '\n') {
        error = "Invalid chunked download";
        complete = false;
      }
    }
  } else if (contentLength >= 0) {
    complete = receiveBytes((uint32_t)contentLength);
    if (!complete && error.length() == 0) error = "Incomplete file download";
  } else {
    uint8_t buffer[512];
    uint16_t buffered = 0;
    while (true) {
      int value = readByte(client, ConnectTimeoutMs);
      if (value < 0) {
        if (!client.connected() && !client.available()) break;
        error = "File download timed out";
        complete = false;
        break;
      }
      buffer[buffered++] = (uint8_t)value;
      if (buffered == sizeof(buffer)) {
        if (!writeBytes(buffer, buffered)) {
          complete = false;
          break;
        }
        buffered = 0;
      }
    }
    if (complete && buffered && !writeBytes(buffer, buffered)) complete = false;
  }

  if (!complete) {
    browserLog(String(F("download failed after bytes=")) + String(total) + F(": ") + error);
    removeTemp();
    return false;
  }
  if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    file.close();
    SpiBusLock::unlock();
  } else {
    error = "SD card is busy";
    removeTemp();
    return false;
  }
  if (!isSupportedDownloadFile(tempPath, format)) {
    browserLog(F("download rejected: file signature does not match response format"));
    error = "Unsupported file download";
    removeTemp();
    return false;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    error = "SD card is busy";
    removeTemp();
    return false;
  }
  bool committed = !SD.exists(path) && SD.rename(tempPath.c_str(), path.c_str());
  if (!committed) SD.remove(tempPath.c_str());
  SpiBusLock::unlock();
  if (!committed) {
    browserLog(F("download commit failed"));
    error = "Cannot save file download";
    return false;
  }
  downloadedPath = path;
  browserLog(String(F("download saved: bytes=")) + String(total) + F(", path=") + path);
  return true;
}

String BrowserScreen::resolveUrl(const Url &base, const String &reference) const {
  return BrowserTransport::resolveUrl(base, reference);
}

String BrowserScreen::cookieHeaderFor(const Url &url) const {
  return _persistentState.cookieHeaderFor(url, nowEpoch());
}

void BrowserScreen::acceptCookie(const String &header, const Url &url) {
  _persistentState.acceptCookie(header, url, nowEpoch());
}

void BrowserScreen::pruneCookies() {
  _persistentState.pruneCookies(nowEpoch());
}

uint64_t BrowserScreen::nowEpoch() const {
  time_t value = time(nullptr);
  return value > 1577836800 ? (uint64_t)value : 0;
}

void BrowserScreen::renderDocument(const String &html) {
  // Pixel buffers belong to BrowserScreen until BrowserImageWorker is
  // extracted. Retire them before parsing or starting a future TLS request;
  // BrowserDocument::clear() itself never frees thumbnail memory.
  releaseEmbeddedImages();
  _editingFormControl = -1;
  _document.parseHtml(html, {_imageGeneration, this, resolveDocumentUrl}, documentLayout());
  _title = _document.title();
  browserLog(String(F("document rendered: html bytes=")) + String(html.length()) +
             F(", links=") + String(_document.linkCount()) +
             F(", image placeholders=") + String(_document.imageCount()));
}
void BrowserScreen::renderPlainText(const String &text, const String &title) {
  releaseEmbeddedImages();
  _document.parsePlainText(text, title, documentLayout(), DefaultTextColor, TFT_WHITE);
  _title = _document.title();
  _editingFormControl = -1;
  browserLog(String(F("plain text rendered: bytes=")) + String(text.length()) +
             F(", rows=") + String(_document.rowCount()));
}

void BrowserScreen::clearRenderedDocumentStorage() {
  _document.clear();
  _editingFormControl = -1;
}

void BrowserScreen::drawThumbnail(int16_t imageIndex, int16_t x, int16_t y) const {
  const EmbeddedImage *image = imageIndex >= 0 ? _document.image(imageIndex) : nullptr;
  if (!image || !image->pixels || !image->width || !image->height) return;
  _display.tft().pushImage(x, y, image->width, image->height, image->pixels);
}

void BrowserScreen::drawFormControl(uint16_t rowIndex, int16_t y) {
  const VisualRow *rowRecord = _document.row(rowIndex);
  if (!rowRecord || rowRecord->formControlIndex < 0) return;
  const VisualRow &row = *rowRecord;
  const uint8_t controlIndex = static_cast<uint8_t>(row.formControlIndex);
  const FormControl *controlRecord = _document.formControl(controlIndex);
  if (!controlRecord) return;
  TFT_eSPI &tft = _display.tft();
  const FormControl &control = *controlRecord;
  const int16_t x = DocumentSidePadding;
  const int16_t width = _display.width() - DocumentSidePadding * 2;
  const int16_t height = _document.rowHeight(rowIndex, documentLayout());
  const uint16_t border = control.disabled ? TFT_DARKGREY : TFT_DARKCYAN;
  const uint16_t fill = control.disabled ? TFT_LIGHTGREY : TFT_WHITE;
  const uint16_t textColor = TFT_DARKGREY;
  const uint16_t rowBackground = row.style.hasBlockBackground ? row.style.blockBackgroundColor
    : (row.style.hasBackground ? row.style.backgroundColor : _document.backgroundColor());
  const UiText::Size labelSize = control.labelIsPlaceholder
    ? UiText::Size::Compact : row.style.size;
  const String label = UiText::ellipsize(_document.formControlLabel(controlIndex), width - 14, labelSize);

  if (control.kind == FormControlKind::Submit || control.kind == FormControlKind::Reset) {
    const uint16_t buttonColor = control.disabled ? TFT_DARKGREY :
      (control.kind == FormControlKind::Submit ? TFT_BLUE : TFT_DARKCYAN);
    tft.fillSmoothRoundRect(x, y + 4, width, height - 8, 5, buttonColor, rowBackground);
    const String centered = UiText::ellipsize(label, width - 14, UiText::Size::Content);
    UiText::draw(tft, centered, x + width / 2, y + height / 2,
                 TFT_WHITE, buttonColor, UiText::Size::Content, MC_DATUM);
    return;
  }
  if (control.kind == FormControlKind::Checkbox || control.kind == FormControlKind::Radio) {
    const int16_t markerX = x + 7;
    const int16_t markerY = y + (height - 18) / 2;
    if (control.kind == FormControlKind::Checkbox) {
      tft.drawRoundRect(markerX, markerY, 18, 18, 3, border);
      if (control.checked) {
        tft.fillRoundRect(markerX + 3, markerY + 3, 12, 12, 2, TFT_BLUE);
        tft.drawLine(markerX + 5, markerY + 9, markerX + 8, markerY + 13, TFT_WHITE);
        tft.drawLine(markerX + 8, markerY + 13, markerX + 14, markerY + 5, TFT_WHITE);
      }
    } else {
      tft.drawCircle(markerX + 9, markerY + 9, 8, border);
      if (control.checked) tft.fillCircle(markerX + 9, markerY + 9, 4, TFT_BLUE);
    }
    UiText::draw(tft, UiText::ellipsize(label, width - 34, labelSize), markerX + 25,
                 y + (height - UiText::lineHeight(labelSize)) / 2, textColor,
                 _document.backgroundColor(), labelSize);
    return;
  }
  tft.fillRoundRect(x, y + 2, width, height - 4, 4, fill);
  tft.drawRoundRect(x, y + 2, width, height - 4, 4, border);
  UiText::draw(tft, label, x + 7, y + 6, textColor, fill, labelSize);
  String value = _document.formControlValue(controlIndex);
  if (control.kind == FormControlKind::Unsupported) value = "Unsupported";
  const UiText::Size valueSize = row.style.size;
  value = UiText::ellipsize(value, width - 30, valueSize);
  UiText::draw(tft, value, x + 7, y + 23, control.disabled ? TFT_DARKGREY : TFT_BLACK, fill, valueSize);
  if (control.kind == FormControlKind::Select) {
    tft.fillTriangle(x + width - 18, y + height / 2 - 3, x + width - 8, y + height / 2 - 3,
                     x + width - 13, y + height / 2 + 4, control.disabled ? TFT_DARKGREY : TFT_DARKCYAN);
  }
}

bool BrowserScreen::resolveDocumentUrl(void *context, const String &reference, String &resolved) {
  BrowserScreen *screen = static_cast<BrowserScreen *>(context);
  if (!screen) return false;
  Url base;
  String error;
  if (!screen->parseUrl(screen->_currentUrl, base)) return false;
  const String candidate = screen->resolveUrl(base, reference);
  return screen->normalizeUrl(candidate, resolved, error);
}

bool BrowserScreen::fetchEmbeddedImageOnWorker(const String &url, uint16_t backgroundColor,
                                               bool useSessionCache, uint16_t *&pixels,
                                               uint8_t &width, uint8_t &height) {
  pixels = nullptr;
  width = height = 0;

  String sourcePath = useSessionCache ? sessionImagePath(url) : "";
  bool cachedSource = sourcePath.length() > 0;
  if (cachedSource) browserLog(String(F("session image cache hit: ")) + url);

  if (!cachedSource) {
    String finalUrl;
    String body;
    String contentType;
    String downloadedPath;
    String error;
    if (!fetch({HttpMethod::Get, url, ""}, finalUrl, body, contentType, downloadedPath, error,
               MaxEmbeddedImageBytes) ||
        downloadedPath.length() == 0) {
      browserLog(String(F("image fetch failed: ")) + error);
      return false;
    }
    const FileFormats::Format downloadedFormat = FileFormats::fromPath(downloadedPath);
    if (FileFormats::group(downloadedFormat) != FileFormats::Group::Image) {
      if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
        SD.remove(downloadedPath.c_str());
        SpiBusLock::unlock();
      }
      return false;
    }
    sourcePath = adoptSessionImage(url, downloadedPath, downloadedFormat);
    cachedSource = sourcePath.length() > 0;
    if (!cachedSource) sourcePath = downloadedPath;
  }

  const FileFormats::Format sourceFormat = FileFormats::fromPath(sourcePath);
  if (FileFormats::group(sourceFormat) != FileFormats::Group::Image) {
    if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
      if (!cachedSource) SD.remove(sourcePath.c_str());
      SpiBusLock::unlock();
    }
    return false;
  }

  // PNGdec contains a 32 KiB row buffer in this build and JPEGDEC has another
  // sizeable source buffer. Keeping a static decoder placed both in internal
  // RAM for the whole browser lifetime, starving mbedTLS after image activity.
  // Construct it in PSRAM for this single worker job and release it before the
  // next HTTPS request. Do not place it on the task stack.
  void *decoderStorage = psramFound() ? ps_malloc(sizeof(ThumbnailDecoder)) : malloc(sizeof(ThumbnailDecoder));
  if (!decoderStorage) {
    browserLog(String(F("image decoder allocation failed: bytes=")) + String(sizeof(ThumbnailDecoder)) +
               F(", freeHeap=") + String(ESP.getFreeHeap()) + F(", freePsram=") + String(ESP.getFreePsram()));
    return false;
  }
  ThumbnailDecoder *decoder = new (decoderStorage) ThumbnailDecoder();
  SpiBusLock::lock();
  const bool decoded = decoder->decode(sourcePath, sourceFormat, pixels, width, height, backgroundColor);
  SpiBusLock::unlock();
  decoder->~ThumbnailDecoder();
  free(decoderStorage);
  if (!cachedSource && SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    SD.remove(sourcePath.c_str());
    SpiBusLock::unlock();
  }
  if (!decoded || !pixels || !width || !height) {
    if (cachedSource) discardSessionImage(url);
    freeThumbnailPixels(pixels, F("image decode failure"));
    pixels = nullptr;
    return false;
  }
  browserLog(String(F("image decoded: ")) + String(width) + F("x") + String(height));
  return true;
}

void BrowserScreen::releaseEmbeddedImages() {
  const uint16_t imageCount = _document.imageCount();
  if (imageCount) browserLog(String(F("releasing image jobs and previews: count=")) + String(imageCount));
  // Invalidate the generation before touching the previews.  The worker owns
  // a decoded buffer until it has handed it to _imageResults; accepting that
  // buffer after the document has been discarded creates two independent
  // cleanup paths for the same allocation (worker result cleanup and the
  // next document's preview cleanup).
  _imageGeneration++;
  if (_imageGeneration == 0) _imageGeneration = 1;

  // Jobs which have not started belong to the document being discarded and
  // have no heap payload.  Removing them means a rapid Back/navigation
  // sequence cannot make the worker start an already-invalid request.
  if (_imageJobs) {
    ImageJob job{};
    while (xQueueReceive(_imageJobs, &job, 0) == pdTRUE) {}
  }

  // Results already waiting in the queue still own their pixel buffer.  Free
  // them here while this generation is being retired instead of letting a
  // later screen process stale ownership.
  if (_imageResults) {
    ImageResult result{};
    while (xQueueReceive(_imageResults, &result, 0) == pdTRUE) {
      freeThumbnailPixels(result.pixels, F("cancelled result queue"));
    }
  }

  for (uint16_t i = 0; i < imageCount; ++i) {
    freeThumbnailPixels(_document.detachImagePixels(i), F("cancelled preview"));
  }
  _document.clearImageDescriptors();
  _nextImageToQueue = 0;
  _deferCachedPageDraw = false;
  _deferCachedPageDrawStartedMs = 0;
}

void BrowserScreen::pumpImageJobs() {
  if (!_imageJobs || _showStart || _message.length()) return;
  while (_nextImageToQueue < _document.imageCount()) {
    const uint16_t index = _nextImageToQueue++;
    ImageJob job{};
    // The image cache belongs to the browser session, not one cached page.
    // A shared icon on the next site can therefore decode locally too.
    if (!_document.beginImageJob(index, _imageGeneration, _sessionCacheActive, job)) continue;
    if (xQueueSend(_imageJobs, &job, 0) != pdTRUE) {
      _document.returnImageJobToPending(index, _imageGeneration);
      _nextImageToQueue--;
      return;
    }
    browserLog(String(F("image job queued #")) + String(index + 1));
  }
}

void BrowserScreen::collectImageResults() {
  if (!_imageResults) return;
  bool redraw = false;
  bool redrawCurrentPage = false;
  const bool wasLoading = isLoading();
  ImageResult result{};
  while (xQueueReceive(_imageResults, &result, 0) == pdTRUE) {
    if (result.generation == _imageGeneration && result.pixels && result.width && result.height &&
        _document.setImageResult(result.imageIndex, result.generation, result.pixels,
                                 result.width, result.height)) {
      if (imageIsVisibleOnCurrentPage(result.imageIndex)) redrawCurrentPage = true;
      browserLog(String(F("image preview ready #")) + String(result.imageIndex + 1) + F(" (") +
                 String(result.width) + F("x") + String(result.height) + F(")"));
      redraw = true;
      continue;
    }
    if (result.generation == _imageGeneration &&
        _document.markImageFailed(result.imageIndex, result.generation)) {
      browserLog(String(F("image preview failed #")) + String(result.imageIndex + 1));
    }
    freeThumbnailPixels(result.pixels, F("stale image result"));
  }
  if (redraw && !_deferCachedPageDraw && !_showStart && _message.length() == 0) {
    reflowDocument();
    // Do not repaint a page that has no visible placeholder to replace. It
    // avoids a perceptible flash while images below or above this page load.
    if (redrawCurrentPage) redrawDocument();
  }
  if (wasLoading != isLoading() && !_showStart) {
    BrowserDisplayLock lock;
    if (lock.locked()) drawControls();
  }
}

bool BrowserScreen::cachedPageImagesComplete() const {
  return _nextImageToQueue >= _document.imageCount() &&
         _activeImageGeneration != _imageGeneration &&
         !_document.hasUnfinishedImages(_imageGeneration);
}

bool BrowserScreen::imageIsVisibleOnCurrentPage(uint16_t imageIndex) const {
  return _document.imageVisibleOnPage(_page, imageIndex);
}

void BrowserScreen::imageWorkerTask(void *context) {
  BrowserScreen *screen = static_cast<BrowserScreen *>(context);
  ImageJob job{};
  while (true) {
    if (xQueueReceive(screen->_imageJobs, &job, portMAX_DELAY) != pdTRUE) continue;
    // releaseEmbeddedImages() removes pending jobs, but the worker may have
    // already dequeued one just as Back was pressed.  Do not give that old
    // document a second route to a pixel-buffer free.
    if (job.generation != screen->_imageGeneration) continue;

    ImageResult result{};
    result.imageIndex = job.imageIndex;
    result.generation = job.generation;
    browserLog(String(F("image worker started #")) + String(job.imageIndex + 1));
    screen->_activeImageGeneration = job.generation;
    screen->fetchEmbeddedImageOnWorker(String(job.url), job.backgroundColor, job.useSessionCache,
                                       result.pixels, result.width, result.height);
    screen->_activeImageGeneration = 0;
    browserLog(String(F("image worker finished #")) + String(job.imageIndex + 1));

    // A cancellation can happen while a JPEG/PNG/GIF decoder is finishing.
    // Keep the buffer on this worker in that case; the UI thread has already
    // released the old document and must never receive it.
    if (job.generation != screen->_imageGeneration) {
      freeThumbnailPixels(result.pixels, F("cancelled worker result"));
      continue;
    }

    // Avoid parking an obsolete result (and its heap allocation) forever if
    // the queue is full while the user navigates away.
    while (xQueueSend(screen->_imageResults, &result, pdMS_TO_TICKS(50)) != pdTRUE) {
      if (job.generation != screen->_imageGeneration) {
        freeThumbnailPixels(result.pixels, F("cancelled queued result"));
        result.pixels = nullptr;
        break;
      }
    }
  }
}

void BrowserScreen::reflowDocument() {
  _document.reflow(documentLayout());
  if (_page >= _document.pageCount()) _page = 0;
}

bool BrowserScreen::hasNextPage() const {
  return _page + 1 < _document.pageCount();
}

String BrowserScreen::shortText(const String &value, int16_t maxWidth) const {
  return BrowserText::ellipsize(_display.tft(), value, maxWidth);
}

String BrowserScreen::lower(const String &value) const {
  String result = value;
  result.toLowerCase();
  return result;
}

int16_t BrowserScreen::imageWidth(const String &label) const {
  (void)label;
  return 26 + BrowserText::measure(ImagePlaceholderLabel);
}

void BrowserScreen::addHistory(const String &url) {
  if (url.length() == 0) return;
  if (_historyCount >= MaxHistory) {
    for (uint8_t index = 1; index < MaxHistory; index++) {
      _historyUrls[index - 1] = _historyUrls[index];
    }
    _historyCount = MaxHistory - 1;
  }
  _historyUrls[_historyCount++] = url;
  browserLog(String(F("history stored: entries=")) + String(_historyCount));
}

void BrowserScreen::addRecent(const String &url, const String &title) {
  _persistentState.addRecent(url, title);
  browserLog(String(F("recent site saved: entries=")) + String(_persistentState.recentCount()));
}

uint8_t BrowserScreen::startSiteCount() const {
  return _showFavorites ? _persistentState.favoriteCount() : _persistentState.recentCount();
}

const BrowserScreen::RecentSite &BrowserScreen::startSiteAt(uint8_t index) const {
  return _showFavorites ? _persistentState.favoriteAt(index) : _persistentState.recentAt(index);
}

void BrowserScreen::clampStartListScroll() {
  const uint8_t count = startSiteCount();
  if (count == 0) {
    _startListScroll = 0;
    return;
  }
  const uint8_t finalPage = ((count - 1) / StartListVisibleRows) * StartListVisibleRows;
  if (_startListScroll > finalPage) _startListScroll = finalPage;
  else _startListScroll = (_startListScroll / StartListVisibleRows) * StartListVisibleRows;
}

bool BrowserScreen::isFavorite(const String &url) const {
  return url.length() && _persistentState.isFavorite(url);
}

void BrowserScreen::addFavorite(const String &url, const String &title) {
  if (url.length() == 0 || isFavorite(url)) return;
  if (_persistentState.favoriteCount() >= MaxRecentSites) {
    browserLog(F("favorite site ignored: list is full"));
    return;
  }
  _persistentState.addFavorite(url, title);
  browserLog(String(F("favorite site saved: entries=")) + String(_persistentState.favoriteCount()));
}

void BrowserScreen::removeRecent(uint8_t index) {
  if (index >= _persistentState.recentCount()) return;
  _persistentState.removeRecent(index);
  clampStartListScroll();
  browserLog(String(F("recent site removed: entries=")) + String(_persistentState.recentCount()));
}

void BrowserScreen::removeFavorite(const String &url) {
  if (!isFavorite(url)) return;
  _persistentState.removeFavorite(url);
  clampStartListScroll();
  browserLog(String(F("favorite site removed: entries=")) + String(_persistentState.favoriteCount()));
}

void BrowserScreen::toggleFavorite() {
  if (_currentUrl.length() == 0) return;
  if (isFavorite(_currentUrl)) removeFavorite(_currentUrl);
  else addFavorite(_currentUrl, _title);
  saveState();
  _menuOpen = false;
  draw();
}

void BrowserScreen::toggleUserAgent() {
  _androidUserAgent = !_androidUserAgent;
  _menuOpen = false;
  browserLog(_androidUserAgent ? F("Android user agent enabled") : F("ESP user agent enabled"));
  // Re-fetch the same URL without touching Browser history or the persistent
  // recent list, so the changed identity is reflected immediately.
  openUrl(_currentUrl, false, false, false);
}

void BrowserScreen::loadState() {
  _persistentState.load();
}

void BrowserScreen::saveState() {
  _persistentState.save();
}
