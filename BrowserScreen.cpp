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

// JPEGDEC's filesystem overload refers to File without its fs namespace.
using File = fs::File;
#include <JPEGDEC.h>
#include <PNGdec.h>
#include "third_party/AnimatedGIF/AnimatedGIF.h"

#include "Navigator.h"
#include "SdCardController.h"
#include "SpiBusLock.h"
#include "UiKit.h"
#include "WiFiController.h"
#include "icons.h"

namespace {
const int16_t HeaderHeight = UiKit::AppBarHeight;
const int16_t ControlsHeight = 42;
const int16_t TextRowHeight = 22;
const int16_t FormControlHeight = 42;
const int16_t FormTextareaHeight = 58;
const int16_t DocumentPadding = 7;
const int16_t DocumentSidePadding = 8;
const uint8_t ThumbnailMaxWidth = 200;
const uint8_t ThumbnailMaxHeight = 200;
const uint32_t MaxEmbeddedImageBytes = 384UL * 1024UL;
const int16_t StartOpenButtonTop = HeaderHeight + 8;
const int16_t StartOpenButtonHeight = 32;
const int16_t RecentListTop = StartOpenButtonTop + StartOpenButtonHeight + 24;
const uint16_t DefaultTextColor = TFT_BLACK;
const uint16_t ImagePlaceholderColor = TFT_DARKGREY;
const uint16_t LinkImageColor = TFT_BLUE;
const char *StatePath = "/.system/browser_state.txt";
const char *StateTempPath = "/.system/browser_state.tmp";
const char *DownloadDirectory = "/Downloads";
const char *SessionCacheDirectory = "/.system/browser_session";

void browserLog(const __FlashStringHelper *message) {
  Serial.print(F("[Browser] "));
  Serial.println(message);
}

void browserLog(const String &message) {
  Serial.print(F("[Browser] "));
  Serial.println(message);
}

String trimCopy(const String &value) {
  String result = value;
  result.trim();
  return result;
}

String clientState(WiFiClient &client) {
  return String(F("connected=")) + (client.connected() ? F("yes") : F("no")) +
         F(", available=") + String(client.available());
}

bool startsWithIgnoreCase(const String &value, const char *prefix) {
  size_t length = strlen(prefix);
  return value.length() >= length && value.substring(0, length).equalsIgnoreCase(prefix);
}

void drawFooterArrow(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                     bool vertical, bool forward, uint16_t color) {
  const int16_t shaft = 10;
  const int16_t wing = 7;
  for (int8_t offset = -1; offset <= 1; offset++) {
    if (vertical) {
      int16_t pointY = centerY + (forward ? wing : -wing);
      int16_t tailY = centerY - (forward ? wing : -wing);
      tft.drawLine(centerX + offset, tailY, centerX + offset, pointY, color);
      tft.drawLine(centerX + offset, pointY, centerX - wing + offset, pointY - (forward ? wing : -wing), color);
      tft.drawLine(centerX + offset, pointY, centerX + wing + offset, pointY - (forward ? wing : -wing), color);
    } else {
      int16_t pointX = centerX + (forward ? wing : -wing);
      int16_t tailX = centerX - shaft;
      if (!forward) tailX = centerX + shaft;
      tft.drawLine(tailX, centerY + offset, pointX, centerY + offset, color);
      tft.drawLine(pointX, centerY + offset, pointX - (forward ? wing : -wing), centerY - wing + offset, color);
      tft.drawLine(pointX, centerY + offset, pointX - (forward ? wing : -wing), centerY + wing + offset, color);
    }
  }
}

void drawFooterHome(TFT_eSPI &tft, int16_t centerX, int16_t centerY, uint16_t color) {
  const int16_t left = centerX - 10;
  const int16_t right = centerX + 10;
  const int16_t roof = centerY - 9;
  const int16_t base = centerY + 10;
  for (int8_t offset = -1; offset <= 1; offset++) {
    tft.drawLine(left + offset, centerY - 1, centerX + offset, roof, color);
    tft.drawLine(centerX + offset, roof, right + offset, centerY - 1, color);
    tft.drawLine(left + offset, centerY - 1, left + offset, base, color);
    tft.drawLine(right + offset, centerY - 1, right + offset, base, color);
    tft.drawLine(left + offset, base, right + offset, base, color);
  }
  tft.fillRect(centerX - 2, centerY + 3, 5, 8, color);
}

void drawFooterClose(TFT_eSPI &tft, int16_t centerX, int16_t centerY, uint16_t color) {
  for (int8_t offset = -1; offset <= 1; offset++) {
    tft.drawLine(centerX - 7, centerY - 7 + offset, centerX + 7, centerY + 7 + offset, color);
    tft.drawLine(centerX - 7, centerY + 7 + offset, centerX + 7, centerY - 7 + offset, color);
  }
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
    if (_pixels) free(_pixels);
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
    if (!_owner || !draw) return 0;
    const size_t lineBytes = (size_t)draw->iWidth * sizeof(uint16_t);
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
    _plainClient(),
    _secureClient(),
    _formCount(0),
    _formControlCount(0),
    _formOptionCount(0),
    _editingFormControl(-1),
    _linkCount(0),
    _historyCount(0),
    _recentCount(0),
    _cookieCount(0),
    _loaded(false),
    _lifecycleLoading(false),
    _lifecycleDrawPending(false),
    _lifecycleGeneration(0),
    _showStart(true),
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
    _sessionCacheActive(false),
    _sessionCacheInitialized(false),
    _usingSessionPage(false),
    _deferCachedPageDraw(false),
    _sessionCacheBytes(0),
    _sessionCacheClock(0),
    _sessionCacheSequence(0),
    _documentBackgroundColor(TFT_WHITE),
    _titleBuffer(),
    _rowCount(0),
    _embeddedImageCount(0),
    _nextImageToQueue(0),
    _imageGeneration(1),
    _activeImageGeneration(0),
    _imageJobs(xQueueCreate(ImageJobQueueLength, sizeof(ImageJob))),
    _imageResults(xQueueCreate(ImageJobQueueLength, sizeof(ImageResult))),
    _imageWorker(nullptr),
    _lifecycleJobs(xQueueCreate(LifecycleJobQueueLength, sizeof(LifecycleJob))),
    _lifecycleWorker(nullptr),
    _httpMutex(xSemaphoreCreateMutex()),
    _pageCount(0),
    _currentStyle{DefaultTextColor, TFT_WHITE, false, TFT_WHITE, false, false, false, false, false, 0, false},
    _styleDepth(0),
    _cssRuleCount(0),
    _cssSourceOrder(0),
    _pendingSpace(false),
    _renderedCharacters(0),
    _closeBounds{0, 4, UiKit::NavigationIconSize, UiKit::NavigationIconSize},
    _backBounds{0, 0, 0, 0},
    _homeBounds{0, 0, 0, 0},
    _openBounds{0, 0, 0, 0},
    _previousBounds{0, 0, 0, 0},
    _nextBounds{0, 0, 0, 0} {
  for (uint16_t i = 0; i < MaxEmbeddedImages; i++) {
    _embeddedImages[i] = {nullptr, 0, 0, TFT_WHITE, "", 0, 0};
  }
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    _recentBounds[i] = {0, 0, 0, 0};
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
  if (_lifecycleJobs) {
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
    const BaseType_t workerCore = 0;
#else
    const BaseType_t workerCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
    xTaskCreatePinnedToCore(lifecycleWorkerTask, "browser-startup", 6144, this,
                            tskIDLE_PRIORITY + 1, &_lifecycleWorker, workerCore);
  } else {
    browserLog(F("browser startup worker unavailable"));
  }
}

void BrowserScreen::onEnter() {
  browserLog(F("entered"));
  _lastTouchDown = false;
  // Draw the interactive shell before any SD work.  The lifecycle worker
  // loads persistent state and clears the previous session cache on the
  // other core; this avoids holding the Home screen during Browser entry.
  if ((!_sessionCacheActive || !_loaded) && !_lifecycleLoading) startLifecycleLoad();
  draw();
}

void BrowserScreen::onResult(const ScreenResult &result) {
  if (_editingFormControl >= 0) {
    if (result.kind == ScreenResultKind::Text && _editingFormControl < _formControlCount) {
      _formControls[_editingFormControl].value = result.text;
      draw();
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
  if (_lifecycleDrawPending) {
    _lifecycleDrawPending = false;
    draw();
  }
  collectImageResults();
  pumpImageJobs();
  if (_deferCachedPageDraw && cachedPageImagesComplete()) {
    _deferCachedPageDraw = false;
    updateImageLayout();
    buildPages();
    finishLoading();
    browserLog(F("cached page images ready; drawing page"));
    draw();
  }
}

void BrowserScreen::startLifecycleLoad() {
  _lifecycleGeneration++;
  if (_lifecycleGeneration == 0) _lifecycleGeneration = 1;
  _lifecycleLoading = true;
  _lifecycleDrawPending = false;
  // Do not touch the SD card here.  A failed queue allocation still leaves a
  // useful Browser shell; state can be loaded on a later entry.
  LifecycleJob job{_lifecycleGeneration};
  if (!_lifecycleJobs || xQueueSend(_lifecycleJobs, &job, 0) != pdTRUE) {
    _lifecycleLoading = false;
    browserLog(F("browser startup job could not be queued"));
  }
}

void BrowserScreen::lifecycleWorkerTask(void *context) {
  BrowserScreen *screen = static_cast<BrowserScreen *>(context);
  LifecycleJob job{};
  while (true) {
    if (xQueueReceive(screen->_lifecycleJobs, &job, portMAX_DELAY) != pdTRUE) continue;
    if (job.generation != screen->_lifecycleGeneration) continue;

    // A browser session never needs its old temporary pages after it has
    // been closed.  Performing this potentially long recursive SD cleanup
    // here keeps both opening and closing Browser responsive.
    screen->_sessionCacheActive = true;
    screen->_sessionCacheInitialized = false;
    {
      HttpLock lock(screen->_httpMutex);
      if (lock.locked() && job.generation == screen->_lifecycleGeneration) {
        screen->_sessionCacheInitialized = screen->clearSessionCacheLocked();
      }
    }
    if (job.generation == screen->_lifecycleGeneration) {
      if (!screen->_loaded) {
        screen->loadState();
        screen->_loaded = true;
      }
      screen->_lifecycleLoading = false;
      screen->_lifecycleDrawPending = true;
      browserLog(F("browser startup work complete"));
    }
  }
}

void BrowserScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

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
    _title = "";
    _message = "";
    _rowCount = 0;
    _pageCount = 0;
    _page = 0;
    _showStart = true;
    _navigator.pop();
    return;
  }
  if (_lifecycleLoading) return;
  if (contains(_backBounds, point.x, point.y)) {
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
    draw();
    return;
  }
  if (_showStart && contains(_openBounds, point.x, point.y)) {
    browserLog(F("opening URL input"));
    openUrlInput();
    return;
  }
  if (!_showStart && contains(_previousBounds, point.x, point.y)) {
    if (_page > 0) {
      _page--;
      browserLog(String(F("moved to page ")) + String(_page + 1));
    }
    draw();
    return;
  }
  if (!_showStart && contains(_nextBounds, point.x, point.y)) {
    if (hasNextPage()) {
      _page++;
      browserLog(String(F("moved to page ")) + String(_page + 1));
    }
    draw();
    return;
  }
  if (_showStart) {
    for (uint8_t i = 0; i < MaxRecentSites; i++) {
      if (contains(_recentBounds[i], point.x, point.y)) {
        browserLog(String(F("opening recent site #")) + String(i + 1));
        openUrl(_recentSites[i].url);
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
      browserLog(F("opening document link"));
      openUrl(linkUrl);
    }
  }
}

void BrowserScreen::layout() {
  const int16_t width = _display.width();
  const int16_t height = _display.height();
  const int16_t buttonWidth = width / 4;
  _closeBounds = {0, 4, UiKit::NavigationIconSize, UiKit::NavigationIconSize};
  _backBounds = {0, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _homeBounds = {buttonWidth, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _openBounds = {70, StartOpenButtonTop, (int16_t)(width - 140), StartOpenButtonHeight};
  _previousBounds = {(int16_t)(buttonWidth * 2), (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _nextBounds = {(int16_t)(buttonWidth * 3), (int16_t)(height - ControlsHeight),
                 (int16_t)(width - buttonWidth * 3), ControlsHeight};
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    _recentBounds[i] = {0, 0, 0, 0};
  }
  const int16_t footerTop = height - ControlsHeight;
  const uint8_t capacity = footerTop > RecentListTop
    ? (uint8_t)((footerTop - RecentListTop) / UiKit::ListCellHeight) : 0;
  const uint8_t visible = _lifecycleLoading ? 0 : (_recentCount < capacity ? _recentCount : capacity);
  for (uint8_t i = 0; i < visible; i++) {
    _recentBounds[i] = {0, (int16_t)(RecentListTop + i * UiKit::ListCellHeight),
                        width, UiKit::ListCellHeight};
  }
}

void BrowserScreen::draw() {
  if (!_display.isReady()) return;
  layout();
  _display.tft().setFreeFont();
  _display.tft().fillScreen(TFT_BLACK);
  drawHeader();
  if (_showStart) drawStartPage();
  else drawDocument();
  drawControls();
}

void BrowserScreen::drawHeader() {
  String heading = _showStart ? "Browser" : (_title.length() ? _title : _currentUrl);
  UiKit::drawTopAppBar(_display.tft(), _display.width(), heading);
}

void BrowserScreen::drawStartPage() {
  TFT_eSPI &tft = _display.tft();
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.fillRoundRect(_openBounds.x, _openBounds.y, _openBounds.w, _openBounds.h, 5, TFT_BLUE);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.drawString("Open URL", _openBounds.x + _openBounds.w / 2, _openBounds.y + _openBounds.h / 2, 2);
  tft.setTextDatum(TL_DATUM);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Recent sites", 10, RecentListTop - 18, 2);
  // Recent-site records are populated by the lifecycle worker.  Avoid
  // reading their Strings until its hand-off frame in tick().
  if (_lifecycleLoading) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Loading saved browser data...", 10, RecentListTop + 10, 2);
    return;
  }
  if (_recentCount == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("No recent sites yet", 10, RecentListTop + 10, 2);
    return;
  }
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    if (_recentBounds[i].w == 0) break;
    const RecentSite &site = _recentSites[i];
    const Rect &bounds = _recentBounds[i];
    String label = site.title.length() ? site.title : site.url;
    UiKit::drawListCell(tft, bounds.w, bounds.y, label);
  }
}

void BrowserScreen::drawDocument() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, HeaderHeight, _display.width(),
               _display.height() - HeaderHeight - ControlsHeight, _documentBackgroundColor);
  const int16_t textTop = HeaderHeight + DocumentPadding;
  if (_foregroundLoading) {
    drawLoadingPanel(true, true);
    return;
  }
  if (_message.length() > 0) {
    const String hint = shortText(_message, _display.width() - 28);
    const int16_t hintWidth = tft.textWidth(hint, 2) + 14;
    const int16_t hintX = (_display.width() - hintWidth) / 2;
    const uint16_t hintBackground = TFT_RED;
    const uint16_t hintForeground = TFT_WHITE;
    tft.fillRoundRect(hintX, textTop + 7, hintWidth, 26, 5, hintBackground);
    tft.setTextColor(hintForeground, hintBackground);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(hint, _display.width() / 2, textTop + 20, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }
  if (_rowCount == 0) {
    tft.setTextColor(TFT_DARKGREY, _documentBackgroundColor);
    tft.drawString("Page has no readable text", 10, textTop, 2);
    return;
  }
  uint16_t start = _page < _pageCount ? _pageStarts[_page] : 0;
  uint16_t end = pageEndRow(_page);
  int16_t y = textTop;
  for (uint16_t index = start; index < end; index++) {
    const VisualRow &row = _rows[index];
    if (row.formControlIndex >= 0) {
      drawFormControl(row, y);
    } else if (row.image) {
      uint16_t color = row.imageLink ? LinkImageColor : ImagePlaceholderColor;
      const uint16_t rowBackground = row.style.hasBlockBackground ? row.style.blockBackgroundColor : _documentBackgroundColor;
      const int16_t height = rowHeight(row);
      int16_t width = imageWidth(row.imageLabel);
      const bool hasThumbnail = row.imageIndex >= 0 && row.imageIndex < _embeddedImageCount &&
                                _embeddedImages[row.imageIndex].pixels;
      if (hasThumbnail) {
        const EmbeddedImage &image = _embeddedImages[row.imageIndex];
        width = image.width;
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
        applyFont(row.style);
        tft.setTextColor(color, row.style.hasBackground ? row.style.backgroundColor : rowBackground);
        tft.setTextDatum(ML_DATUM);
        tft.drawString(shortImageLabel(row.imageLabel, width - 26), DocumentSidePadding + 22, y + TextRowHeight / 2);
        tft.setTextDatum(TL_DATUM);
      }
    } else {
      const uint16_t rowBackground = row.style.hasBlockBackground ? row.style.blockBackgroundColor : _documentBackgroundColor;
      if (row.style.hasBlockBackground) {
        tft.fillRect(DocumentSidePadding, y, _display.width() - DocumentSidePadding * 2, rowHeight(row),
                     row.style.blockBackgroundColor);
      }
      for (uint8_t runIndex = 0; runIndex < row.runCount; runIndex++) {
        const VisualRun &run = row.runs[runIndex];
        const int16_t x = DocumentSidePadding + run.x;
        if (run.image) {
          uint16_t color = run.style.linkIndex ? LinkImageColor : ImagePlaceholderColor;
          const int16_t height = run.imageIndex >= 0 && run.imageIndex < _embeddedImageCount &&
                                 _embeddedImages[run.imageIndex].pixels
            ? _embeddedImages[run.imageIndex].height : TextRowHeight;
          if (run.style.hasBackground) tft.fillRect(x, y, run.width, height, run.style.backgroundColor);
          const bool hasThumbnail = run.imageIndex >= 0 && run.imageIndex < _embeddedImageCount &&
                                    _embeddedImages[run.imageIndex].pixels;
          if (hasThumbnail) {
            drawThumbnail(run.imageIndex, x, y);
          } else {
            tft.drawXBitmap(x + 2, y + 3, ic_file, 16, 16, color);
            applyFont(run.style);
            tft.setTextColor(color, run.style.hasBackground ? run.style.backgroundColor : rowBackground);
            tft.setTextDatum(ML_DATUM);
            tft.drawString(run.text, x + 22, y + TextRowHeight / 2);
            tft.setTextDatum(TL_DATUM);
          }
          continue;
        }
        applyFont(run.style);
        if (run.style.hasBackground && run.width > 0) {
          tft.fillRect(x, y, run.width, TextRowHeight, run.style.backgroundColor);
        }
        tft.setTextColor(run.style.color, run.style.hasBackground ? run.style.backgroundColor : rowBackground);
        tft.setTextDatum(TL_DATUM);
        tft.drawString(run.text, x, y);
        if (run.style.underline && run.width > 0) {
          tft.drawFastHLine(x, y + TextRowHeight - 2, run.width, run.style.color);
        }
      }
    }
    y += rowHeight(row);
  }
  tft.setFreeFont();
}

void BrowserScreen::drawLoadingPanel(bool redrawPhase, bool drawBarFrame) {
  TFT_eSPI &tft = _display.tft();
  const int16_t textTop = HeaderHeight + DocumentPadding;
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
  const int16_t barY = textTop + 54;
  const int16_t barWidth = _display.width() - barX * 2;

  if (redrawPhase) {
    tft.fillRect(0, textTop, _display.width(), 27, _documentBackgroundColor);
    const String phase = shortText(_loadingPhase.length() ? _loadingPhase : String(F("Loading")),
                                   _display.width() - 28);
    tft.setTextColor(TFT_BLACK, _documentBackgroundColor);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(phase, _display.width() / 2, textTop + 18, 2);
  }
  // Only this short line and the bar interior change during a transfer. This
  // leaves the loading title and the bar frame on screen, eliminating flicker.
  tft.fillRect(0, textTop + 28, _display.width(), 21, _documentBackgroundColor);
  tft.setTextColor(TFT_DARKGREY, _documentBackgroundColor);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(detail, _display.width() / 2, textTop + 39, 2);
  if (knownSize && _lastLoadingKnownSize && !drawBarFrame) {
    UiKit::updateProgressBar(tft, barX, barY, barWidth, 12, _lastLoadingPercent, percent,
                             TFT_BLUE, _documentBackgroundColor);
  } else {
    UiKit::drawProgressBar(tft, barX, barY, barWidth, 12, percent, !knownSize,
                           TFT_BLUE, TFT_DARKGREY, _documentBackgroundColor, drawBarFrame,
                           millis() - _loadingStartedMs);
  }
  _lastLoadingPercent = percent;
  _lastLoadingKnownSize = knownSize;
  tft.setTextDatum(TL_DATUM);
}

void BrowserScreen::redrawDocument() {
  if (!_display.isReady()) return;
  drawDocument();
}

void BrowserScreen::drawControls() {
  TFT_eSPI &tft = _display.tft();
  const Rect buttons[] = {_backBounds, _homeBounds, _previousBounds, _nextBounds};
  const bool loading = isLoading();
  for (uint8_t i = 0; i < 4; i++) {
    bool active = (i == 0 && (loading || !_showStart)) || (i == 1 && !_showStart) ||
      (!_showStart && ((i == 2 && _page > 0) ||
      (i == 3 && hasNextPage())));
    uint16_t color = active ? 0x2965 : TFT_DARKGREY;
    const Rect &button = buttons[i];
    tft.fillRect(button.x, button.y, button.w - 1, button.h, color);
    uint16_t iconColor = active ? TFT_WHITE : 0x8410;
    const int16_t centerX = button.x + button.w / 2;
    const int16_t centerY = button.y + button.h / 2;
    if (i == 0 && loading) drawFooterClose(tft, centerX, centerY, iconColor);
    else if (i == 0) drawFooterArrow(tft, centerX, centerY, false, false, iconColor);
    else if (i == 1) drawFooterHome(tft, centerX, centerY, iconColor);
    else drawFooterArrow(tft, centerX, centerY, true, i == 3, iconColor);
  }
  tft.setTextDatum(TL_DATUM);
}

bool BrowserScreen::contains(const Rect &rect, int16_t x, int16_t y) const {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

bool BrowserScreen::isLoading() const {
  if (_foregroundLoading) return true;
  if (_activeImageGeneration == _imageGeneration) return true;
  for (uint16_t i = 0; i < _embeddedImageCount; i++) {
    if (_embeddedImages[i].generation == _imageGeneration && _embeddedImages[i].state < 2) return true;
  }
  return false;
}

void BrowserScreen::beginLoading(const String &phase) {
  _foregroundLoading = true;
  _documentBackgroundColor = TFT_WHITE;
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
  HttpLock lock(_httpMutex, 0);
  if (!lock.locked()) {
    browserLog(F("network client stop deferred to active request"));
    return;
  }
  _plainClient.stop();
  _secureClient.stop();
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
  if (_message.length() || _page >= _pageCount) return "";
  const uint16_t start = _pageStarts[_page];
  const uint16_t end = pageEndRow(_page);
  int16_t rowY = HeaderHeight + DocumentPadding;
  for (uint16_t index = start; index < end; index++) {
    const VisualRow &row = _rows[index];
    const int16_t height = rowHeight(row);
    if (y >= rowY && y < rowY + height) {
      if (row.image) {
        int16_t width = imageWidth(row.imageLabel);
        if (row.imageIndex >= 0 && row.imageIndex < _embeddedImageCount) {
          width = _embeddedImages[row.imageIndex].width;
        }
        bool insideImage = x >= DocumentSidePadding && x < DocumentSidePadding + width;
        return insideImage && row.imageLinkIndex ? _linkUrls[row.imageLinkIndex - 1] : "";
      }
      for (uint8_t runIndex = 0; runIndex < row.runCount; runIndex++) {
        const VisualRun &run = row.runs[runIndex];
        if (run.style.linkIndex && x >= DocumentSidePadding + run.x &&
            x < DocumentSidePadding + run.x + run.width) {
          return _linkUrls[run.style.linkIndex - 1];
        }
      }
      return "";
    }
    rowY += height;
  }
  return "";
}

int8_t BrowserScreen::documentFormControlAt(int16_t x, int16_t y) const {
  if (_message.length() || _page >= _pageCount) return -1;
  const uint16_t start = _pageStarts[_page];
  const uint16_t end = pageEndRow(_page);
  int16_t rowY = HeaderHeight + DocumentPadding;
  for (uint16_t index = start; index < end; index++) {
    const VisualRow &row = _rows[index];
    const int16_t height = rowHeight(row);
    if (y >= rowY && y < rowY + height) {
      return row.formControlIndex >= 0 && x >= DocumentSidePadding &&
             x < _display.width() - DocumentSidePadding ? row.formControlIndex : -1;
    }
    rowY += height;
  }
  return -1;
}

void BrowserScreen::openFormTextInput(uint8_t index) {
  if (index >= _formControlCount) return;
  const FormControl &control = _formControls[index];
  String title = "Edit " + formControlLabel(control);
  if (_navigator.push(ScreenId::TextInput,
                      ScreenData::textInput(shortText(title, 120), false, control.value))) {
    _editingFormControl = index;
  }
}

void BrowserScreen::resetForm(uint8_t formIndex) {
  for (uint8_t i = 0; i < _formControlCount; i++) {
    FormControl &control = _formControls[i];
    if (control.formIndex != formIndex) continue;
    control.value = control.defaultValue;
    control.checked = control.defaultChecked;
    control.selectedOption = control.defaultSelectedOption;
  }
  draw();
}

void BrowserScreen::handleFormControl(uint8_t index) {
  if (index >= _formControlCount) return;
  FormControl &control = _formControls[index];
  if (control.disabled || control.kind == FormControlKind::Unsupported) return;
  switch (control.kind) {
    case FormControlKind::Text:
    case FormControlKind::Textarea:
      openFormTextInput(index);
      return;
    case FormControlKind::Checkbox:
      control.checked = !control.checked;
      draw();
      return;
    case FormControlKind::Radio:
      if (!control.checked) {
        for (uint8_t i = 0; i < _formControlCount; i++) {
          FormControl &candidate = _formControls[i];
          if (candidate.kind == FormControlKind::Radio && candidate.formIndex == control.formIndex &&
              candidate.name == control.name) candidate.checked = false;
        }
        control.checked = true;
        draw();
      }
      return;
    case FormControlKind::Select:
      if (control.optionCount) {
        control.selectedOption = (control.selectedOption + 1) % control.optionCount;
        draw();
      }
      return;
    case FormControlKind::Reset:
      if (control.formIndex >= 0) resetForm((uint8_t)control.formIndex);
      return;
    case FormControlKind::Submit:
      if (control.formIndex >= 0) submitForm((uint8_t)control.formIndex, index);
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
  if (formIndex >= _formCount) return;
  const FormData &form = _forms[formIndex];
  if (!form.getMethod) {
    _message = "This form uses POST; only GET is supported";
    draw();
    return;
  }
  Url base;
  if (!parseUrl(_currentUrl, base)) {
    _message = "Form action could not be resolved";
    draw();
    return;
  }
  String target = resolveUrl(base, form.action);
  String query;
  for (uint8_t i = 0; i < _formControlCount; i++) {
    const FormControl &control = _formControls[i];
    if (control.formIndex != formIndex || control.disabled || !control.name.length()) continue;
    if (control.kind == FormControlKind::Submit && i != submitControlIndex) continue;
    if (control.kind == FormControlKind::Reset || control.kind == FormControlKind::Unsupported) continue;
    if ((control.kind == FormControlKind::Checkbox || control.kind == FormControlKind::Radio) && !control.checked) continue;
    String value = formControlValue(control);
    if (query.length()) query += '&';
    query += formUrlEncode(control.name) + '=' + formUrlEncode(value);
  }
  if (query.length()) target += target.indexOf('?') >= 0 ? '&' : '?';
  target += query;
  browserLog(String(F("submitting GET form: ")) + target);
  openUrl(target, true, false);
}

void BrowserScreen::openUrlInput() {
  _navigator.push(ScreenId::TextInput, ScreenData::textInput("Open URL", false));
}

void BrowserScreen::resetSessionCacheEntries() {
  _sessionCacheBytes = 0;
  _sessionCacheClock = 0;
  _sessionCacheSequence = 0;
  for (uint8_t i = 0; i < MaxSessionPages; i++) {
    _sessionPages[i] = {"", "", "", 0, 0, false};
  }
  for (uint8_t i = 0; i < MaxSessionImages; i++) {
    _sessionImages[i] = {"", "", 0, 0, false};
  }
}

void BrowserScreen::endSessionCache() {
  // This is called from the close button.  Cache cleanup can include many
  // FAT deletes, and an image worker can still own one of those files, so
  // leave cleanup for startLifecycleLoad() instead of blocking the UI task.
  _lifecycleGeneration++;
  if (_lifecycleGeneration == 0) _lifecycleGeneration = 1;
  _lifecycleLoading = false;
  _lifecycleDrawPending = false;
  _sessionCacheInitialized = false;
  _sessionCacheActive = false;
  _usingSessionPage = false;
  browserLog(F("session cache cleanup deferred"));
}

bool BrowserScreen::initializeSessionCacheLocked() {
  if (!_sessionCacheActive || !_sdCard.isMounted()) return false;
  if (_sessionCacheInitialized) return true;
  // A navigation made during the first displayed Browser frame should still
  // work, but it must not steal the cache's potentially long setup from the
  // UI task.  It simply proceeds without a session-cache write this time.
  if (_lifecycleLoading) return false;
  _sessionCacheInitialized = clearSessionCacheLocked();
  return _sessionCacheInitialized;
}

bool BrowserScreen::clearSessionCacheLocked() {
  resetSessionCacheEntries();
  if (!_sdCard.isMounted() || !SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  File directory = SD.open(SessionCacheDirectory, FILE_READ);
  if (directory && directory.isDirectory()) {
    File item = directory.openNextFile();
    while (item) {
      String path = item.name();
      item.close();
      SD.remove(path.c_str());
      item = directory.openNextFile();
    }
    directory.close();
    SD.rmdir(SessionCacheDirectory);
  } else if (directory) {
    directory.close();
    SD.remove(SessionCacheDirectory);
  }
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

bool BrowserScreen::ensureSessionCacheSpaceLocked(uint32_t bytes) {
  if (bytes > MaxSessionCacheBytes) return false;
  while (_sessionCacheBytes > MaxSessionCacheBytes - bytes) {
    if (!evictOldestSessionEntryLocked()) return false;
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
  HttpLock lock(_httpMutex);
  if (!lock.locked() || !initializeSessionCacheLocked()) return false;
  const int8_t index = sessionPageIndex(url);
  if (index < 0) return false;
  SessionPageCacheEntry &entry = _sessionPages[index];
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  File file = SD.open(entry.path.c_str(), FILE_READ);
  const uint32_t size = file ? (uint32_t)file.size() : 0xffffffffUL;
  SpiBusLock::unlock();
  if (!file || size != entry.bytes || size > MaxBodyBytes || !body.reserve(size)) {
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
    if (removeSessionFileLocked(entry.path)) removeSessionPageLocked((uint8_t)index);
    body = "";
    return false;
  }
  entry.lastUse = nextSessionCacheUse();
  contentType = entry.contentType;
  return true;
}

bool BrowserScreen::storeSessionPage(const String &url, const String &contentType, const String &body) {
  if (body.length() > MaxBodyBytes) return false;
  HttpLock lock(_httpMutex);
  if (!lock.locked() || !initializeSessionCacheLocked()) return false;
  int8_t existing = sessionPageIndex(url);
  if (existing >= 0) {
    if (!removeSessionFileLocked(_sessionPages[existing].path)) return false;
    removeSessionPageLocked((uint8_t)existing);
  }
  if (!ensureSessionCacheSpaceLocked(body.length())) return false;
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
  if (!file) return false;
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
  bool committed = written && !SD.exists(path.c_str()) && SD.rename(temporary.c_str(), path.c_str());
  if (!committed) SD.remove(temporary.c_str());
  SpiBusLock::unlock();
  if (!committed) return false;
  _sessionPages[slot] = {url, contentType, path, body.length(), nextSessionCacheUse(), true};
  _sessionCacheBytes += body.length();
  browserLog(String(F("page stored in session cache: ")) + url);
  return true;
}

String BrowserScreen::sessionImagePath(const String &url) {
  HttpLock lock(_httpMutex);
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
  HttpLock lock(_httpMutex);
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
  HttpLock lock(_httpMutex);
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
  if (!ensureSessionCacheSpaceLocked(bytes)) return "";
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
  const bool hadDocument = !_showStart && _currentUrl.length() > 0;
  const String previousUrl = _currentUrl;
  String url;
  String error;
  if (!normalizeUrl(value, url, error)) {
    browserLog(String(F("URL rejected: ")) + error);
    _showStart = false;
    _message = error;
    _rowCount = 0;
    _pageCount = 0;
    draw();
    return false;
  }
  browserLog(String(F("navigation requested: ")) + url);
  if (!useSessionCache) _usingSessionPage = false;
  String finalUrl = url;
  String body;
  String contentType;
  const bool cacheHit = useSessionCache && loadSessionPage(url, body, contentType);
  if (cacheHit) {
    browserLog(String(F("session cache hit: ")) + url);
  } else if (!_wifi.isConnected()) {
    browserLog(F("navigation blocked: Wi-Fi is disconnected"));
    _showStart = false;
    _message = "Connect to Wi-Fi first";
    _rowCount = 0;
    _pageCount = 0;
    draw();
    return false;
  }

  // Record the page we are leaving before its replacement starts loading.
  // A cancelled (or failed) request still leaves that page reachable through
  // Back; delaying this until after fetch() succeeds made the footer Back
  // button disappear after cancelling a navigation.
  if (addToHistory && hadDocument && previousUrl != url) {
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
  _rowCount = 0;
  _pageCount = 0;
  _title = "";
  _page = 0;
  _loadCancelled = false;
  _usingSessionPage = cacheHit;
  _deferCachedPageDraw = false;
  // Navigation can be initiated by the Back button itself.  That tap remains
  // down while this synchronous request starts, so treat it as already
  // consumed; otherwise pollLoadingCancel() cancels the Back request at once.
  _loadCancelTouchDown = _lastTouchDown;
  beginLoading(cacheHit ? F("Restoring cached page") : F("Preparing request"));
  draw();
  yield();

  String downloadedPath;
  if (!cacheHit && !fetch(url, finalUrl, body, contentType, downloadedPath, error)) {
    browserLog(String(F("navigation failed: url=")) + url + F(", phase=") + _loadingPhase +
               F(", error=") + error + F(", userCancelled=") +
               (_loadCancelled ? F("yes") : F("no")) + F(", activeImageGeneration=") +
               String(_activeImageGeneration) + F(", currentImageGeneration=") + String(_imageGeneration));
    finishLoading();
    _message = _loadCancelled ? "Loading cancelled" : error;
    draw();
    return false;
  }
  _currentUrl = finalUrl;
  setLoadingPhase(cacheHit ? F("Rendering cached page")
                            : (downloadedPath.length() ? F("Opening downloaded file") : F("Rendering page")));
  if (downloadedPath.length()) {
    FileFormats::Format format = FileFormats::fromPath(downloadedPath);
    FileFormats::Group group = FileFormats::group(format);
    renderPlainText(group == FileFormats::Group::Audio
      ? "Downloaded audio opened in player."
      : (group == FileFormats::Group::Video ? "Downloaded video opened in player."
         : "Downloaded image opened in viewer."),
      group == FileFormats::Group::Audio ? "Audio download"
      : (group == FileFormats::Group::Video ? "Video download" : "Image download"));
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
    _message = "Could not open downloaded file";
    draw();
    return true;
  }
  const bool htmlPage = contentType.indexOf("text/html") >= 0 ||
                        contentType.indexOf("application/xhtml") >= 0 || contentType.length() == 0;
  const bool textPage = contentType.indexOf("text/plain") >= 0;
  if (!cacheHit && (htmlPage || textPage)) {
    storeSessionPage(_currentUrl, contentType, body);
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
  if (cacheHit && _embeddedImageCount > 0 && _imageJobs && _imageResults) {
    _deferCachedPageDraw = true;
    setLoadingPhase(F("Loading cached images"));
    browserLog(String(F("waiting for cached images: ")) + String(_embeddedImageCount));
    return true;
  }
  finishLoading();
  browserLog(String(F("page ready: title=\"")) + _title + F("\", rows=") + String(_rowCount) +
             F(", pages=") + String(_pageCount) + F(", images=") + String(_embeddedImageCount));
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
  normalized = trimCopy(value);
  error = "";
  if (normalized.length() == 0) {
    error = "Enter a URL";
    return false;
  }
  if (normalized.indexOf("://") < 0) normalized = "https://" + normalized;
  Url url;
  if (!parseUrl(normalized, url)) {
    error = "Only valid HTTP/HTTPS URLs work";
    return false;
  }
  normalized = url.scheme + "://" + url.host;
  bool defaultPort = (url.scheme == "http" && url.port == 80) || (url.scheme == "https" && url.port == 443);
  if (!defaultPort) normalized += ":" + String(url.port);
  normalized += url.path;
  return true;
}

bool BrowserScreen::parseUrl(const String &value, Url &url) const {
  int schemeEnd = value.indexOf("://");
  if (schemeEnd <= 0) return false;
  url.scheme = lower(value.substring(0, schemeEnd));
  if (url.scheme != "http" && url.scheme != "https") return false;
  int authorityStart = schemeEnd + 3;
  int pathStart = value.indexOf('/', authorityStart);
  int queryStart = value.indexOf('?', authorityStart);
  if (pathStart < 0 || (queryStart >= 0 && queryStart < pathStart)) pathStart = queryStart;
  String authority = pathStart < 0 ? value.substring(authorityStart) : value.substring(authorityStart, pathStart);
  if (authority.length() == 0 || authority.indexOf('@') >= 0) return false;
  int colon = authority.lastIndexOf(':');
  url.port = url.scheme == "https" ? 443 : 80;
  url.host = authority;
  if (colon >= 0) {
    String portText = authority.substring(colon + 1);
    long port = portText.toInt();
    if (port <= 0 || port > 65535 || String(port) != portText) return false;
    url.port = (uint16_t)port;
    url.host = authority.substring(0, colon);
  }
  url.host.trim();
  if (url.host.length() == 0 || url.host.indexOf(' ') >= 0 || url.host.indexOf('[') >= 0) return false;
  url.host = lower(url.host);
  url.path = pathStart < 0 ? "/" : value.substring(pathStart);
  if (url.path.startsWith("?")) url.path = "/" + url.path;
  int fragment = url.path.indexOf('#');
  if (fragment >= 0) url.path.remove(fragment);
  if (url.path.length() == 0) url.path = "/";
  return true;
}

bool BrowserScreen::fetch(const String &initialUrl, String &finalUrl, String &body,
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
  HttpLock lock(_httpMutex, 0);
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
  String next = initialUrl;
  for (uint8_t redirect = 0; redirect <= MaxRedirects; redirect++) {
    Url url;
    if (!parseUrl(next, url)) {
      browserLog(F("invalid redirect target"));
      error = "Invalid redirect URL";
      return false;
    }
    browserLog(String(F("HTTP request ")) + String(redirect + 1) + F("/") + String(MaxRedirects + 1) +
               F(": ") + next);
    HttpResponse response{};
    WiFiClient *client = nullptr;
    if (!requestOne(url, response, client, error)) {
      browserLog(String(F("HTTP request failed: url=")) + next + F(", phase=") + _loadingPhase +
                 F(", error=") + error + F(", userCancelled=") +
                 (_loadCancelled ? F("yes") : F("no")));
      return false;
    }
    if (response.statusCode >= 300 && response.statusCode <= 399) {
      client->stop();
      if (response.location.length() == 0) {
        browserLog(String(F("redirect rejected: url=")) + next + F(", status=") + String(response.statusCode) +
                   F(", reason=no Location header"));
        error = "Redirect has no destination";
        return false;
      }
      next = resolveUrl(url, response.location);
      browserLog(String(F("redirect ")) + String(response.statusCode) + F(" -> ") + next);
      continue;
    }
    if (response.statusCode == 401) {
      client->stop();
      browserLog(String(F("server requires unsupported Basic authentication: url=")) + next);
      error = "Basic authentication is unsupported";
      return false;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
      client->stop();
      browserLog(String(F("HTTP request rejected: url=")) + next + F(", status=") + String(response.statusCode));
      error = "HTTP error " + String(response.statusCode);
      return false;
    }
    contentType = response.contentType;
    FileFormats::Format format = downloadableFormatForResponse(contentType, url);
    bool downloadable = FileFormats::isDownloadable(format);
    browserLog(String(F("response accepted: type=")) +
               (contentType.length() ? contentType : String(F("unspecified"))) +
               F(", length=") + String(response.contentLength) +
               (response.chunked ? F(", chunked") : F("")) +
               (downloadable ? F(", media download") : F(", page response")));
    bool readOk = false;
    if (downloadable && response.compressed) {
      browserLog(String(F("compressed media download rejected: url=")) + next + F(", encoding is not identity"));
      error = "Compressed file response unsupported";
    } else if (downloadable) {
      readOk = readDownloadBody(*client, response.chunked, response.contentLength, format,
                                downloadedPath, error, downloadLimit);
    } else {
      readOk = readBody(*client, response.chunked, response.contentLength, body, error);
    }
    if (!readOk) {
      browserLog(String(F("response transfer failed: url=")) + next + F(", phase=") + _loadingPhase +
                 F(", error=") + error + F(", retainedBytes=") + String(body.length()) + F(", ") +
                 clientState(*client));
      client->stop();
      return false;
    }
    client->stop();
    finalUrl = next;
    browserLog(String(F("HTTP transfer complete: ")) + finalUrl);
    return true;
  }
  browserLog(String(F("redirect limit reached: initialUrl=")) + initialUrl + F(", limit=") + String(MaxRedirects));
  error = "Too many redirects";
  return false;
}

bool BrowserScreen::requestOne(const Url &url, HttpResponse &response, WiFiClient *&client,
                               String &error) {
  if (pollLoadingCancel()) {
    error = "Loading cancelled";
    browserLog(String(F("request cancelled before connect: url=")) + url.scheme + F("://") + url.host + url.path);
    return false;
  }
  _plainClient.stop();
  _secureClient.stop();
  client = &_plainClient;
  if (url.scheme == "https") {
    _secureClient.setInsecure();
    _secureClient.setHandshakeTimeout(ConnectTimeoutMs / 1000);
    client = &_secureClient;
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
    _plainClient.stop();
    rawTcpState = _plainClient.connect(resolvedAddress, url.port, 1000) ? "connected" : "failed";
    _plainClient.stop();
    if (rawTcpState == "connected") {
      _secureClient.stop();
      _secureClient.setHandshakeTimeout(TlsRetryHandshakeTimeoutSeconds);
      browserLog(String(F("HTTPS retrying resolved address: host=")) + url.host + F(", address=") +
                 resolvedAddress.toString() + F(", handshakeTimeoutSeconds=") +
                 String(TlsRetryHandshakeTimeoutSeconds));
      const uint32_t retryConnectStartedMs = millis();
      connected = _secureClient.connect(resolvedAddress, url.port, url.host.c_str(), nullptr, nullptr, nullptr);
      retryConnectElapsedMs = millis() - retryConnectStartedMs;
      if (connected) browserLog(String(F("HTTPS retry via resolved address succeeded: elapsedMs=")) +
                                String(retryConnectElapsedMs));
    }
  }
  if (!connected) {
    char tlsErrorText[96] = {};
    const int tlsError = url.scheme == "https"
      ? _secureClient.lastError(tlsErrorText, sizeof(tlsErrorText)) : 0;
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
  String request = "GET " + url.path + " HTTP/1.1\r\nHost: " + hostHeader +
                   "\r\nUser-Agent: ESP-Media-Browser/1.0\r\nAccept: text/html,*/*;q=0.1\r\n"
                   "Accept-Encoding: identity\r\nConnection: close\r\n";
  String cookies = cookieHeaderFor(url);
  if (cookies.length()) request += "Cookie: " + cookies + "\r\n";
  request += "\r\n";
  const size_t requestBytes = client->print(request);
  if (requestBytes != request.length()) {
    browserLog(String(F("request write failed: host=")) + url.host + F(", sent=") + String(requestBytes) +
               F(", expected=") + String(request.length()) + F(", ") + clientState(*client));
    client->stop();
    error = "Request failed";
    return false;
  }

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
  response.location = "";
  for (uint8_t count = 0; count < 48; count++) {
    if (!readLine(*client, line, ConnectTimeoutMs)) {
      browserLog(String(F("response headers timed out: host=")) + url.host + F(", headerCount=") +
                 String(count) + F(", timeoutMs=") + String(ConnectTimeoutMs) + F(", ") + clientState(*client));
      client->stop();
      error = "Response headers timed out";
      return false;
    }
    if (line.length() == 0) break;
    int colon = line.indexOf(':');
    if (colon <= 0) continue;
    String name = lower(trimCopy(line.substring(0, colon)));
    String headerValue = trimCopy(line.substring(colon + 1));
    if (name == "content-length") response.contentLength = headerValue.toInt();
    else if (name == "transfer-encoding" && lower(headerValue).indexOf("chunked") >= 0) response.chunked = true;
    else if (name == "content-encoding" && !headerValue.equalsIgnoreCase("identity")) response.compressed = true;
    else if (name == "content-type") response.contentType = lower(headerValue);
    else if (name == "location") response.location = headerValue;
    else if (name == "set-cookie") acceptCookie(headerValue, url);
  }
  browserLog(String(F("headers complete: length=")) + String(response.contentLength) +
             F(", type=") + (response.contentType.length() ? response.contentType : String(F("unspecified"))) +
             (response.chunked ? F(", chunked") : F("")) +
             (response.compressed ? F(", compressed") : F("")));
  return true;
}

bool BrowserScreen::readLine(WiFiClient &client, String &line, uint32_t timeoutMs) {
  line = "";
  uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (pollLoadingCancel()) return false;
    if (_activeImageGeneration && _activeImageGeneration != _imageGeneration) return false;
    while (client.available()) {
      int value = client.read();
      if (value < 0) continue;
      if (value == '\n') {
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        return true;
      }
      if (line.length() >= 2048) return false;
      line += (char)value;
    }
    if (!client.connected() && !client.available()) return line.length() > 0;
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
                                     uint32_t downloadLimit) {
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
    path = String(DownloadDirectory) + "/media-" + String((unsigned long)stamp) + "-" + String(attempt) + extension;
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
  String target = trimCopy(reference);
  String root = base.scheme + "://" + base.host;
  bool defaultPort = (base.scheme == "http" && base.port == 80) || (base.scheme == "https" && base.port == 443);
  if (!defaultPort) root += ":" + String(base.port);

  int fragment = target.indexOf('#');
  if (fragment >= 0) target.remove(fragment);
  if (target.length() == 0) return root + base.path;

  if (target.startsWith("//")) return base.scheme + ":" + target;
  if (target.indexOf("://") >= 0) return target;

  int colon = target.indexOf(':');
  int separator = target.length();
  int slash = target.indexOf('/');
  int query = target.indexOf('?');
  if (slash >= 0 && slash < separator) separator = slash;
  if (query >= 0 && query < separator) separator = query;
  if (colon >= 0 && colon < separator) return target;

  if (target.startsWith("/")) return root + target;
  String path = base.path;
  int queryStart = path.indexOf('?');
  if (queryStart >= 0) path.remove(queryStart);
  if (target.startsWith("?")) return root + path + target;
  String directory = path;
  slash = directory.lastIndexOf('/');
  directory = slash >= 0 ? directory.substring(0, slash + 1) : "/";
  return root + directory + target;
}

String BrowserScreen::cookieHeaderFor(const Url &url) const {
  String result;
  for (uint8_t i = 0; i < _cookieCount; i++) {
    const Cookie &cookie = _cookies[i];
    bool hostMatches = url.host == cookie.domain ||
      (url.host.endsWith("." + cookie.domain) && cookie.domain.length() > 0);
    String cookiePath = cookie.path.length() ? cookie.path : "/";
    bool pathMatches = url.path.startsWith(cookiePath) &&
      (cookiePath.endsWith("/") || url.path.length() == cookiePath.length() ||
       url.path[cookiePath.length()] == '/');
    if (!hostMatches || !pathMatches || (cookie.secure && url.scheme != "https")) continue;
    if (result.length()) result += "; ";
    result += cookie.name + "=" + cookie.value;
  }
  return result;
}

void BrowserScreen::acceptCookie(const String &header, const Url &url) {
  int separator = header.indexOf(';');
  String first = trimCopy(separator < 0 ? header : header.substring(0, separator));
  int equals = first.indexOf('=');
  if (equals <= 0) return;
  Cookie incoming;
  incoming.name = trimCopy(first.substring(0, equals));
  incoming.value = first.substring(equals + 1);
  incoming.domain = url.host;
  incoming.path = "/";
  incoming.secure = false;
  incoming.expiresAt = 0;
  int position = separator < 0 ? header.length() : separator + 1;
  while (position < (int)header.length()) {
    int end = header.indexOf(';', position);
    String attribute = trimCopy(header.substring(position, end < 0 ? header.length() : end));
    int attributeEquals = attribute.indexOf('=');
    String name = lower(trimCopy(attributeEquals < 0 ? attribute : attribute.substring(0, attributeEquals)));
    String value = attributeEquals < 0 ? "" : trimCopy(attribute.substring(attributeEquals + 1));
    if (name == "domain" && value.length()) {
      if (value.startsWith(".")) value.remove(0, 1);
      incoming.domain = lower(value);
    } else if (name == "path" && value.startsWith("/")) {
      incoming.path = value;
    } else if (name == "secure") {
      incoming.secure = true;
    } else if (name == "max-age") {
      long seconds = value.toInt();
      if (seconds <= 0) incoming.expiresAt = 1;
      else if (nowEpoch()) incoming.expiresAt = nowEpoch() + (uint64_t)seconds;
    } else if (name == "expires" && !incoming.expiresAt) {
      incoming.expiresAt = parseHttpDate(value);
    }
    if (end < 0) break;
    position = end + 1;
  }
  if (incoming.name.length() == 0) return;
  bool domainMatches = url.host == incoming.domain || url.host.endsWith("." + incoming.domain);
  if (!domainMatches) return;
  for (uint8_t i = 0; i < _cookieCount; i++) {
    if (_cookies[i].name == incoming.name && _cookies[i].domain == incoming.domain && _cookies[i].path == incoming.path) {
      _cookies[i] = incoming;
      pruneCookies();
      return;
    }
  }
  if (_cookieCount < MaxCookies) _cookies[_cookieCount++] = incoming;
  else {
    for (uint8_t i = 1; i < MaxCookies; i++) _cookies[i - 1] = _cookies[i];
    _cookies[MaxCookies - 1] = incoming;
  }
  pruneCookies();
}

void BrowserScreen::pruneCookies() {
  uint64_t now = nowEpoch();
  for (uint8_t i = 0; i < _cookieCount;) {
    if (_cookies[i].expiresAt == 1 || (now && _cookies[i].expiresAt && _cookies[i].expiresAt <= now)) {
      for (uint8_t j = i + 1; j < _cookieCount; j++) _cookies[j - 1] = _cookies[j];
      _cookieCount--;
    } else {
      i++;
    }
  }
}

uint64_t BrowserScreen::nowEpoch() const {
  time_t value = time(nullptr);
  return value > 1577836800 ? (uint64_t)value : 0;
}

void BrowserScreen::renderDocument(const String &html) {
  resetRenderedRows();
  collectCssRules(html);
  _titleBuffer = "";
  _title = "";
  bool inScript = false;
  bool inStyle = false;
  bool inTitle = false;
  bool sawScript = false;
  int8_t currentForm = -1;
  int8_t textareaControl = -1;
  int8_t selectControl = -1;
  int8_t buttonControl = -1;
  String textareaText;
  String optionAttributes;
  String optionText;
  String buttonText;
  String labelIds[MaxFormControls];
  String labelTexts[MaxFormControls];
  bool inLabel = false;
  String labelFor;
  String labelText;
  uint8_t labelCount = 0;
  uint8_t labelStartControl = 0;
  bool inOption = false;
  uint32_t position = 0;
  while (position < html.length()) {
    int nextTag = html.indexOf('<', position);
    String text = nextTag < 0 ? html.substring(position) : html.substring(position, nextTag);
    if (inLabel) labelText += text;
    if (inTitle) _titleBuffer += text;
    else if (textareaControl >= 0) textareaText += text;
    else if (inOption) optionText += text;
    else if (buttonControl >= 0) buttonText += text;
    else if (!inScript && !inStyle) appendText(text);
    if (nextTag < 0) break;
    int endTag = findTagEnd(html, nextTag + 1);
    if (endTag < 0) break;
    String inside = trimCopy(html.substring(nextTag + 1, endTag));
    bool closing = inside.startsWith("/");
    if (closing) inside.remove(0, 1);
    inside.trim();
    int space = inside.indexOf(' ');
    String tag = lower(space < 0 ? inside : inside.substring(0, space));
    if (tag.endsWith("/")) tag.remove(tag.length() - 1);
    String attributes = space < 0 ? "" : trimCopy(inside.substring(space + 1));
    bool selfClosing = inside.endsWith("/");
    if (tag.startsWith("!") || tag.startsWith("?")) {
      position = endTag + 1;
      continue;
    }
    if (tag == "script") {
      if (!closing) sawScript = true;
      inScript = !closing;
    }
    else if (tag == "style") inStyle = !closing;
    else if (tag == "title") inTitle = !closing;
    else if (!inScript && !inStyle && !inTitle) {
      if (tag == "label") {
        if (closing) {
          const String resolvedLabel = decodeEntities(trimCopy(labelText));
          if (resolvedLabel.length()) {
            if (labelFor.length() && labelCount < MaxFormControls) {
              labelIds[labelCount] = labelFor;
              labelTexts[labelCount++] = resolvedLabel;
            } else if (_formControlCount > labelStartControl) {
              _formControls[_formControlCount - 1].label = resolvedLabel;
            }
          }
          inLabel = false;
          labelFor = "";
          labelText = "";
          popStyle(tag);
        } else {
          inLabel = true;
          labelFor = decodeEntities(attributeValue(attributes, "for"));
          labelText = "";
          labelStartControl = _formControlCount;
          if (!selfClosing) pushStyle(tag, styledForTag(tag, attributes));
        }
      } else if (tag == "form") {
        if (closing) {
          currentForm = -1;
          appendBreak();
        } else {
          currentForm = addForm(attributes);
          appendBreak();
        }
      } else if (tag == "textarea") {
        if (closing) {
          if (textareaControl >= 0 && textareaControl < _formControlCount) {
            FormControl &control = _formControls[textareaControl];
            control.value = decodeEntities(textareaText);
            control.defaultValue = control.value;
          }
          textareaText = "";
          textareaControl = -1;
        } else {
          textareaText = "";
          textareaControl = addFormControl(FormControlKind::Textarea, currentForm, attributes,
                                           styledForTag(tag, attributes));
        }
      } else if (tag == "select") {
        if (closing) {
          if (selectControl >= 0 && selectControl < _formControlCount) {
            FormControl &control = _formControls[selectControl];
            control.defaultSelectedOption = control.selectedOption;
          }
          selectControl = -1;
        } else if (hasAttribute(attributes, "multiple")) {
          appendStub(formStubLabel("select", attributes) + ": multiple select unsupported");
        } else {
          selectControl = addFormControl(FormControlKind::Select, currentForm, attributes,
                                         styledForTag(tag, attributes));
        }
      } else if (tag == "option" && selectControl >= 0) {
        if (closing || selfClosing) {
          addSelectOption((uint8_t)selectControl, optionAttributes, optionText);
          optionAttributes = "";
          optionText = "";
          inOption = false;
        } else {
          optionAttributes = attributes;
          optionText = "";
          inOption = true;
        }
      } else if (tag == "input" && !closing) {
        String type = lower(trimCopy(attributeValue(attributes, "type")));
        if (!type.length()) type = "text";
        const TextStyle controlStyle = styledForTag(tag, attributes);
        if (type == "text" || type == "search" || type == "email" || type == "url" || type == "number") {
          addFormControl(FormControlKind::Text, currentForm, attributes, controlStyle);
        } else if (type == "checkbox") {
          addFormControl(FormControlKind::Checkbox, currentForm, attributes, controlStyle);
        } else if (type == "radio") {
          addFormControl(FormControlKind::Radio, currentForm, attributes, controlStyle);
        } else if (type == "submit") {
          String label = decodeEntities(attributeValue(attributes, "value"));
          if (!label.length()) label = "Submit";
          addFormControl(FormControlKind::Submit, currentForm, attributes, controlStyle, label);
        } else if (type == "reset") {
          String label = decodeEntities(attributeValue(attributes, "value"));
          if (!label.length()) label = "Reset";
          addFormControl(FormControlKind::Reset, currentForm, attributes, controlStyle, label);
        } else if (type != "hidden") {
          appendStub(formStubLabel("input", attributes) + ": unsupported");
        }
      } else if (tag == "button") {
        if (closing) {
          if (buttonControl >= 0 && buttonControl < _formControlCount) {
            String label = decodeEntities(trimCopy(buttonText));
            if (label.length()) _formControls[buttonControl].label = label;
          }
          buttonText = "";
          buttonControl = -1;
        } else {
          String type = lower(trimCopy(attributeValue(attributes, "type")));
          if (!type.length()) type = "submit";
          FormControlKind kind = type == "submit" ? FormControlKind::Submit :
                                 (type == "reset" ? FormControlKind::Reset : FormControlKind::Unsupported);
          if (kind == FormControlKind::Unsupported) {
            appendStub(formStubLabel("button", attributes) + ": unsupported");
          } else {
            buttonText = "";
            buttonControl = addFormControl(kind, currentForm, attributes, styledForTag(tag, attributes));
          }
        }
      } else if (closing) {
        if (isBlockTag(tag)) appendBreak();
        if (tag == "td" || tag == "th") _pendingSpace = true;
        popStyle(tag);
      } else if (tag == "br") {
        appendBreak();
      } else if (tag == "img") {
        String alt = decodeEntities(attributeValue(attributes, "alt"));
        String source = decodeEntities(attributeValue(attributes, "src"));
        TextStyle imageStyle = styledForTag(tag, attributes);
        const uint16_t imageBackground = imageStyle.hasBackground ? imageStyle.backgroundColor :
          (imageStyle.hasBlockBackground ? imageStyle.blockBackgroundColor : _documentBackgroundColor);
        int16_t imageIndex = fetchEmbeddedImage(source, imageBackground);
        uint8_t imageLinkIndex = _currentStyle.linkIndex;
      if (imageLinkIndex == 0 && source.length()) {
          Url base;
          String resolved;
          String normalized;
          String error;
          if (parseUrl(_currentUrl, base)) resolved = resolveUrl(base, source);
          if (normalizeUrl(resolved, normalized, error)) imageLinkIndex = registerLink(normalized);
        }
        appendImage(alt.length() ? alt : "image", imageStyle, imageLinkIndex != 0,
                    imageLinkIndex, imageStyle.fullWidthImage, imageIndex);
      } else if (tag == "audio" || tag == "video" || tag == "source") {
        String source = decodeEntities(attributeValue(attributes, "src"));
        TextStyle mediaStyle = styledForTag(tag, attributes);
        Url base;
        String resolved;
        String normalized;
        String error;
        if (source.length() && parseUrl(_currentUrl, base)) resolved = resolveUrl(base, source);
        if (source.length() && normalizeUrl(resolved, normalized, error)) {
          mediaStyle.link = true;
          mediaStyle.linkIndex = registerLink(normalized);
        }
        appendBreak();
        appendWord(source.length() ? String(F("[")) + (tag == "source" ? String("media") : tag) + F("]")
                                  : String(F("(")) + tag + F(")"), mediaStyle);
        appendBreak();
      } else if (tag == "iframe" || tag == "object" || tag == "embed" || tag == "canvas") {
        appendStub(tag);
      } else {
        if (isBlockTag(tag)) appendBreak();
        if ((tag == "td" || tag == "th") && _rowCount) _pendingSpace = true;
        if (!selfClosing) {
          TextStyle tagStyle = styledForTag(tag, attributes);
          if ((tag == "html" || tag == "body") && tagStyle.hasBackground) {
            _documentBackgroundColor = tagStyle.backgroundColor;
          }
          pushStyle(tag, tagStyle);
        }
      }
    }
    position = endTag + 1;
  }
  for (uint8_t labelIndex = 0; labelIndex < labelCount; labelIndex++) {
    for (uint8_t controlIndex = 0; controlIndex < _formControlCount; controlIndex++) {
      if (_formControls[controlIndex].id == labelIds[labelIndex]) {
        _formControls[controlIndex].label = labelTexts[labelIndex];
        break;
      }
    }
  }
  _title = decodeEntities(trimCopy(_titleBuffer));
  if (_title.length() == 0) _title = "Web page";
  if (_rowCount == 0) appendText(sawScript
    ? "(This page is generated by JavaScript. JavaScript is not supported.)"
    : "(This page has no static readable text.)");
  buildPages();
  browserLog(String(F("document rendered: html bytes=")) + String(html.length()) +
             F(", CSS rules=") + String(_cssRuleCount) + F(", links=") + String(_linkCount) +
             F(", image placeholders=") + String(_embeddedImageCount));
}

void BrowserScreen::renderPlainText(const String &text, const String &title) {
  resetRenderedRows();
  _title = title;
  appendText(text);
  if (_rowCount == 0) appendText("(No readable text in this response.)");
  buildPages();
  browserLog(String(F("plain text rendered: bytes=")) + String(text.length()) + F(", rows=") + String(_rowCount));
}

void BrowserScreen::resetRenderedRows() {
  releaseEmbeddedImages();
  clearRenderedDocumentStorage();
  _rowCount = 0;
  _pageCount = 0;
  _documentBackgroundColor = TFT_WHITE;
  _styleDepth = 0;
  _currentStyle = {DefaultTextColor, TFT_WHITE, false, TFT_WHITE, false, false, false, false, false, 0, false};
  _cssRuleCount = 0;
  _cssSourceOrder = 0;
  _linkCount = 0;
  _pendingSpace = false;
  _renderedCharacters = 0;
}

void BrowserScreen::clearRenderedDocumentStorage() {
  for (uint16_t i = 0; i < MaxRows; i++) _rows[i] = {};
  for (uint8_t i = 0; i < MaxLinks; i++) _linkUrls[i] = "";
  for (uint8_t i = 0; i < MaxForms; i++) _forms[i] = {};
  for (uint8_t i = 0; i < MaxFormControls; i++) _formControls[i] = {};
  for (uint8_t i = 0; i < MaxFormOptions; i++) _formOptions[i] = {};
  _formCount = 0;
  _formControlCount = 0;
  _formOptionCount = 0;
  _editingFormControl = -1;
  for (uint8_t i = 0; i < MaxStyleDepth; i++) _styleStack[i] = {};
  for (uint8_t i = 0; i < MaxCssRules; i++) _cssRules[i] = {};
  _titleBuffer = "";
}

void BrowserScreen::appendText(const String &text) {
  String decoded = decodeEntities(text);
  String word;
  for (uint16_t i = 0; i <= decoded.length(); i++) {
    char value = i < decoded.length() ? decoded[i] : ' ';
    if (value != ' ' && value != '\r' && value != '\n' && value != '\t') {
      word += value;
      continue;
    }
    if (word.length()) {
      appendWord(word, _currentStyle);
      word = "";
    }
    if (value == '\n' || value == '\r') appendBreak();
    else _pendingSpace = true;
  }
}

void BrowserScreen::appendBreak() {
  _pendingSpace = false;
  if (_rowCount && !_rows[_rowCount - 1].image && _rows[_rowCount - 1].runCount) {
    _rows[_rowCount - 1].contentWidth = _display.width();
  }
}

void BrowserScreen::appendImage(const String &label, const TextStyle &sourceStyle, bool linked,
                                uint8_t linkIndex, bool fullWidth, int16_t imageIndex) {
  TextStyle style = sourceStyle;
  style.link = linked;
  style.linkIndex = linkIndex;
  if (!fullWidth) {
    appendInlineImage(label, style, imageIndex);
    return;
  }
  appendBreak();
  if (_rowCount >= MaxRows) return;
  VisualRow &row = _rows[_rowCount++];
  row.image = true;
  row.formControlIndex = -1;
  row.imageLabel = label;
  row.imageLink = linked;
  row.imageLinkIndex = linkIndex;
  row.imageIndex = imageIndex;
  row.style = style;
  row.contentWidth = 0;
  row.runCount = 0;
}

void BrowserScreen::appendInlineImage(const String &label, const TextStyle &style, int16_t imageIndex) {
  const int16_t maxWidth = _display.width() - DocumentSidePadding * 2;
  const bool hasImageSlot = imageIndex >= 0 && imageIndex < _embeddedImageCount;
  const bool hasThumbnail = hasImageSlot && _embeddedImages[imageIndex].pixels;
  String displayLabel = hasThumbnail ? "" : shortImageLabel(label, maxWidth - 26);
  int16_t width = hasThumbnail ? _embeddedImages[imageIndex].width
                                : imageWidth(displayLabel);
  beginTextRow();
  if (_rowCount == 0) return;

  VisualRow *row = &_rows[_rowCount - 1];
  if (_pendingSpace && row->contentWidth > 0) {
    const int16_t spaceWidth = textWidth(" ", style);
    if (row->contentWidth + spaceWidth + width > maxWidth || row->runCount + 2 > MaxRunsPerRow) {
      row->contentWidth = _display.width();
      beginTextRow();
      if (_rowCount == 0) return;
      row = &_rows[_rowCount - 1];
    } else {
      appendRun(" ", style);
      row = &_rows[_rowCount - 1];
    }
  }
  _pendingSpace = false;

  if ((row->contentWidth > 0 && row->contentWidth + width > maxWidth) ||
      row->runCount >= MaxRunsPerRow) {
    row->contentWidth = _display.width();
    beginTextRow();
    if (_rowCount == 0) return;
    row = &_rows[_rowCount - 1];
  }
  VisualRun &run = row->runs[row->runCount++];
  run.image = true;
  run.text = displayLabel;
  run.style = style;
  run.imageIndex = imageIndex;
  run.x = row->contentWidth;
  run.width = width;
  row->contentWidth += width;
}

void BrowserScreen::appendStub(const String &label) {
  appendBreak();
  appendText(String("(") + label + ")");
  appendBreak();
}

void BrowserScreen::appendFormControl(uint8_t index, const TextStyle &style) {
  appendBreak();
  if (index >= _formControlCount || _rowCount >= MaxRows) return;
  VisualRow &row = _rows[_rowCount++];
  row = {};
  row.image = false;
  row.formControlIndex = index;
  row.imageIndex = -1;
  row.style = style;
  row.contentWidth = _display.width();
  _pendingSpace = false;
}

int8_t BrowserScreen::addForm(const String &attributes) {
  if (_formCount >= MaxForms) return -1;
  FormData &form = _forms[_formCount];
  form = {};
  form.action = decodeEntities(attributeValue(attributes, "action"));
  String method = lower(trimCopy(attributeValue(attributes, "method")));
  form.getMethod = method.length() == 0 || method == "get";
  return _formCount++;
}

int8_t BrowserScreen::addFormControl(FormControlKind kind, int8_t formIndex, const String &attributes,
                                     const TextStyle &style, const String &fallbackLabel) {
  if (_formControlCount >= MaxFormControls) {
    appendStub("form control limit reached");
    return -1;
  }
  FormControl &control = _formControls[_formControlCount];
  control = {};
  control.kind = kind;
  control.formIndex = formIndex;
  control.label = decodeEntities(fallbackLabel);
  control.name = decodeEntities(attributeValue(attributes, "name"));
  control.id = decodeEntities(attributeValue(attributes, "id"));
  if (!control.label.length()) control.label = decodeEntities(attributeValue(attributes, "aria-label"));
  if (!control.label.length()) control.label = decodeEntities(attributeValue(attributes, "placeholder"));
  if (!control.label.length()) control.label = control.name.length() ? control.name : control.id;
  control.value = decodeEntities(attributeValue(attributes, "value"));
  if ((kind == FormControlKind::Checkbox || kind == FormControlKind::Radio) && !control.value.length()) control.value = "on";
  control.defaultValue = control.value;
  control.checked = hasAttribute(attributes, "checked");
  control.defaultChecked = control.checked;
  control.disabled = hasAttribute(attributes, "disabled");
  control.firstOption = _formOptionCount;
  control.optionCount = 0;
  control.selectedOption = 0;
  control.defaultSelectedOption = 0;
  const uint8_t index = _formControlCount++;
  appendFormControl(index, style);
  return index;
}

void BrowserScreen::addSelectOption(uint8_t controlIndex, const String &attributes, const String &text) {
  if (controlIndex >= _formControlCount || _formOptionCount >= MaxFormOptions) return;
  FormControl &control = _formControls[controlIndex];
  if (control.kind != FormControlKind::Select || control.optionCount >= 255) return;
  FormOption &option = _formOptions[_formOptionCount++];
  option = {};
  option.controlIndex = controlIndex;
  option.label = decodeEntities(trimCopy(text));
  option.value = decodeEntities(attributeValue(attributes, "value"));
  if (!option.value.length()) option.value = option.label;
  const uint8_t optionIndex = control.optionCount++;
  if (hasAttribute(attributes, "selected")) {
    control.selectedOption = optionIndex;
    control.defaultSelectedOption = optionIndex;
  }
}

String BrowserScreen::formControlLabel(const FormControl &control) const {
  String label = control.label;
  if (!label.length()) {
    switch (control.kind) {
      case FormControlKind::Submit: label = "Submit"; break;
      case FormControlKind::Reset: label = "Reset"; break;
      case FormControlKind::Select: label = "Select"; break;
      default: label = "Input"; break;
    }
  }
  return label;
}

String BrowserScreen::formControlValue(const FormControl &control) const {
  if (control.kind == FormControlKind::Select && control.optionCount) {
    const uint16_t optionIndex = control.firstOption + control.selectedOption;
    if (optionIndex < _formOptionCount) return _formOptions[optionIndex].value;
  }
  return control.value;
}

bool BrowserScreen::hasAttribute(const String &attributes, const char *name) const {
  String wanted = lower(String(name));
  uint16_t index = 0;
  while (index < attributes.length()) {
    while (index < attributes.length() && (isspace((unsigned char)attributes[index]) || attributes[index] == '/')) index++;
    const uint16_t start = index;
    while (index < attributes.length() && !isspace((unsigned char)attributes[index]) &&
           attributes[index] != '=' && attributes[index] != '/') index++;
    if (start == index) { index++; continue; }
    const String attributeName = lower(attributes.substring(start, index));
    while (index < attributes.length() && isspace((unsigned char)attributes[index])) index++;
    if (attributeName == wanted) return true;
    if (index < attributes.length() && attributes[index] == '=') {
      index++;
      while (index < attributes.length() && isspace((unsigned char)attributes[index])) index++;
      if (index < attributes.length() && (attributes[index] == '\'' || attributes[index] == '\"')) {
        const char quote = attributes[index++];
        while (index < attributes.length() && attributes[index] != quote) index++;
        if (index < attributes.length()) index++;
      } else {
        while (index < attributes.length() && !isspace((unsigned char)attributes[index])) index++;
      }
    }
  }
  return false;
}

String BrowserScreen::formUrlEncode(const String &value) const {
  const char hex[] = "0123456789ABCDEF";
  String encoded;
  encoded.reserve(value.length() * 3);
  for (uint16_t i = 0; i < value.length(); i++) {
    const uint8_t c = (uint8_t)value[i];
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '*') encoded += (char)c;
    else if (c == ' ') encoded += '+';
    else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 15];
    }
  }
  return encoded;
}

String BrowserScreen::formStubLabel(const String &tag, const String &attributes) const {
  String name = decodeEntities(attributeValue(attributes, "name"));
  if (!name.length()) name = decodeEntities(attributeValue(attributes, "id"));
  if (!name.length()) name = decodeEntities(attributeValue(attributes, "aria-label"));
  if (!name.length() && tag == "form") name = decodeEntities(attributeValue(attributes, "action"));
  if (!name.length() && tag == "input") name = decodeEntities(attributeValue(attributes, "type"));
  name.trim();
  return name.length() ? tag + ": " + name : tag;
}

void BrowserScreen::appendWord(const String &word, const TextStyle &style) {
  if (_renderedCharacters >= 16384) return;
  String remaining = word;
  const int16_t maxWidth = _display.width() - DocumentSidePadding * 2;
  while (remaining.length() && _rowCount < MaxRows) {
    beginTextRow();
    VisualRow &row = _rows[_rowCount - 1];
    if (_pendingSpace && row.contentWidth > 0) {
      int16_t spaceWidth = textWidth(" ", style);
      if (row.contentWidth + spaceWidth >= maxWidth) {
        row.contentWidth = _display.width();
        beginTextRow();
      } else {
        appendRun(" ", style);
      }
    }
    _pendingSpace = false;
    VisualRow &target = _rows[_rowCount - 1];
    int16_t available = maxWidth - target.contentWidth;
    int16_t wholeWidth = textWidth(remaining, style);
    const VisualRun *mergedRun = target.runCount && !target.runs[target.runCount - 1].image &&
                                 stylesEqual(target.runs[target.runCount - 1].style, style)
      ? &target.runs[target.runCount - 1] : nullptr;
    if (mergedRun) {
      wholeWidth = textWidth(mergedRun->text + remaining, style) - mergedRun->width;
    }
    if (wholeWidth <= available) {
      appendRun(remaining, style);
      _renderedCharacters += remaining.length();
      return;
    }
    String part;
    while (part.length() < remaining.length()) {
      String candidate = part + remaining[part.length()];
      int16_t candidateWidth = mergedRun
        ? textWidth(mergedRun->text + candidate, style) - mergedRun->width
        : textWidth(candidate, style);
      if (candidateWidth > available) break;
      part += remaining[part.length()];
    }
    if (part.length() == 0) {
      if (target.contentWidth > 0) {
        target.contentWidth = _display.width();
        continue;
      }
      part = remaining.substring(0, 1);
    }
    appendRun(part, style);
    _renderedCharacters += part.length();
    remaining.remove(0, part.length());
    if (remaining.length()) _rows[_rowCount - 1].contentWidth = _display.width();
  }
}

void BrowserScreen::appendRun(const String &text, const TextStyle &style) {
  if (text.length() == 0 || _rowCount == 0) return;
  VisualRow &row = _rows[_rowCount - 1];
  if (row.runCount && !row.runs[row.runCount - 1].image &&
      stylesEqual(row.runs[row.runCount - 1].style, style)) {
    VisualRun &run = row.runs[row.runCount - 1];
    const int16_t previousWidth = run.width;
    run.text += text;
    // Free-font textWidth() uses the glyph's visible bounds for the final
    // character, while text within a drawString() advances by xAdvance.
    // Summing separate fragments therefore underestimates a merged run and
    // makes the next differently styled run paint over its end.
    run.width = textWidth(run.text, style);
    row.contentWidth += run.width - previousWidth;
  } else if (row.runCount < MaxRunsPerRow) {
    const int16_t width = textWidth(text, style);
    VisualRun &run = row.runs[row.runCount++];
    run.image = false;
    run.text = text;
    run.style = style;
    run.imageIndex = -1;
    run.x = row.contentWidth;
    run.width = width;
    row.contentWidth += width;
  } else {
    row.contentWidth = _display.width();
    beginTextRow();
    appendRun(text, style);
    return;
  }
}

void BrowserScreen::beginTextRow() {
  const int16_t maxWidth = _display.width() - DocumentSidePadding * 2;
  if (_rowCount && !_rows[_rowCount - 1].image && _rows[_rowCount - 1].contentWidth < maxWidth) return;
  if (_rowCount >= MaxRows) return;
  VisualRow &row = _rows[_rowCount++];
  row.image = false;
  row.formControlIndex = -1;
  row.imageLabel = "";
  row.imageLink = false;
  row.imageLinkIndex = 0;
  row.imageIndex = -1;
  row.style = _currentStyle;
  row.contentWidth = 0;
  row.runCount = 0;
}

int16_t BrowserScreen::rowHeight(const VisualRow &row) const {
  if (row.formControlIndex >= 0 && row.formControlIndex < _formControlCount) {
    return _formControls[row.formControlIndex].kind == FormControlKind::Textarea
      ? FormTextareaHeight : FormControlHeight;
  }
  int16_t height = TextRowHeight;
  if (row.image) {
    if (row.imageIndex >= 0 && row.imageIndex < _embeddedImageCount) {
      const EmbeddedImage &image = _embeddedImages[row.imageIndex];
      if (image.pixels) height = image.height;
    }
  } else {
    for (uint8_t i = 0; i < row.runCount; i++) {
      const VisualRun &run = row.runs[i];
      if (!run.image || run.imageIndex < 0 || run.imageIndex >= _embeddedImageCount) continue;
      const EmbeddedImage &image = _embeddedImages[run.imageIndex];
      if (!image.pixels) continue;
      const int16_t imageHeight = image.height;
      if (imageHeight > height) height = imageHeight;
    }
  }
  return height > TextRowHeight ? height : TextRowHeight;
}

void BrowserScreen::drawThumbnail(int16_t imageIndex, int16_t x, int16_t y) const {
  if (imageIndex < 0 || imageIndex >= _embeddedImageCount) return;
  const EmbeddedImage &image = _embeddedImages[imageIndex];
  if (!image.pixels || !image.width || !image.height) return;
  _display.tft().pushImage(x, y, image.width, image.height, image.pixels);
}

void BrowserScreen::drawFormControl(const VisualRow &row, int16_t y) {
  if (row.formControlIndex < 0 || row.formControlIndex >= _formControlCount) return;
  TFT_eSPI &tft = _display.tft();
  const FormControl &control = _formControls[row.formControlIndex];
  const int16_t x = DocumentSidePadding;
  const int16_t width = _display.width() - DocumentSidePadding * 2;
  const int16_t height = rowHeight(row);
  const uint16_t border = control.disabled ? TFT_DARKGREY : TFT_DARKCYAN;
  const uint16_t fill = control.disabled ? TFT_LIGHTGREY : TFT_WHITE;
  const uint16_t textColor = control.disabled ? TFT_DARKGREY : row.style.color;
  const String label = shortText(formControlLabel(control), width - 14);

  if (control.kind == FormControlKind::Submit || control.kind == FormControlKind::Reset) {
    const uint16_t buttonColor = control.disabled ? TFT_DARKGREY :
      (control.kind == FormControlKind::Submit ? TFT_BLUE : TFT_DARKCYAN);
    tft.fillRoundRect(x, y + 4, width, height - 8, 5, buttonColor);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_WHITE, buttonColor);
    tft.drawString(label, x + width / 2, y + height / 2, 2);
    tft.setTextDatum(TL_DATUM);
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
    tft.setTextColor(textColor, _documentBackgroundColor);
    tft.setTextDatum(ML_DATUM);
    tft.drawString(shortText(label, width - 34), markerX + 25, y + height / 2, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }
  tft.fillRoundRect(x, y + 2, width, height - 4, 4, fill);
  tft.drawRoundRect(x, y + 2, width, height - 4, 4, border);
  tft.setTextColor(textColor, fill);
  tft.drawString(label, x + 7, y + 6, 2);
  String value = formControlValue(control);
  if (control.kind == FormControlKind::Unsupported) value = "Unsupported";
  if (!value.length() && control.kind != FormControlKind::Select) value = "Tap to enter";
  value = shortText(value, width - 30);
  tft.setTextColor(control.disabled ? TFT_DARKGREY : TFT_BLACK, fill);
  tft.drawString(value, x + 7, y + 23, 2);
  if (control.kind == FormControlKind::Select) {
    tft.fillTriangle(x + width - 18, y + height / 2 - 3, x + width - 8, y + height / 2 - 3,
                     x + width - 13, y + height / 2 + 4, control.disabled ? TFT_DARKGREY : TFT_DARKCYAN);
  }
}

int16_t BrowserScreen::fetchEmbeddedImage(const String &source, uint16_t backgroundColor) {
  if (source.length() == 0 || _embeddedImageCount >= MaxEmbeddedImages ||
      source.length() >= ImageUrlMaxLength) return -1;
  Url base;
  String resolved;
  String url;
  String error;
  if (!parseUrl(_currentUrl, base)) return -1;
  resolved = resolveUrl(base, source);
  if (!normalizeUrl(resolved, url, error)) return -1;

  // Reuse a preview only when it will be composited against the same page
  // color; transparent PNG/GIF pixels are resolved by the worker.
  for (uint16_t i = 0; i < _embeddedImageCount; i++) {
    if (_embeddedImages[i].generation == _imageGeneration && _embeddedImages[i].url == url &&
        _embeddedImages[i].backgroundColor == backgroundColor) return i;
  }

  const uint16_t index = _embeddedImageCount++;
  _embeddedImages[index] = {nullptr, 0, 0, backgroundColor, url, _imageGeneration, 0};
  browserLog(String(F("image placeholder registered #")) + String(index + 1));
  return index;
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
    if (!fetch(url, finalUrl, body, contentType, downloadedPath, error, MaxEmbeddedImageBytes) ||
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
    if (pixels) free(pixels);
    pixels = nullptr;
    return false;
  }
  browserLog(String(F("image decoded: ")) + String(width) + F("x") + String(height));
  return true;
}

void BrowserScreen::releaseEmbeddedImages() {
  if (_embeddedImageCount) browserLog(String(F("releasing image jobs and previews: count=")) + String(_embeddedImageCount));
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
      if (result.pixels) free(result.pixels);
    }
  }

  for (uint16_t i = 0; i < _embeddedImageCount; i++) {
    if (_embeddedImages[i].pixels) free(_embeddedImages[i].pixels);
    _embeddedImages[i] = {nullptr, 0, 0, TFT_WHITE, "", 0, 0};
  }
  _embeddedImageCount = 0;
  _nextImageToQueue = 0;
  _deferCachedPageDraw = false;
}

void BrowserScreen::updateImageLayout() {
  for (uint16_t rowIndex = 0; rowIndex < _rowCount; rowIndex++) {
    VisualRow &row = _rows[rowIndex];
    if (row.image) continue;
    int16_t x = 0;
    for (uint8_t runIndex = 0; runIndex < row.runCount; runIndex++) {
      VisualRun &run = row.runs[runIndex];
      if (run.image && run.imageIndex >= 0 && run.imageIndex < _embeddedImageCount) {
        const EmbeddedImage &image = _embeddedImages[run.imageIndex];
        if (image.pixels) run.width = image.width;
      }
      run.x = x;
      x += run.width;
    }
    row.contentWidth = x;
  }
}

void BrowserScreen::pumpImageJobs() {
  if (!_imageJobs || _showStart || _message.length()) return;
  while (_nextImageToQueue < _embeddedImageCount) {
    EmbeddedImage &image = _embeddedImages[_nextImageToQueue];
    const uint16_t index = _nextImageToQueue++;
    if (image.state != 0 || image.generation != _imageGeneration || image.url.length() >= ImageUrlMaxLength) continue;
    ImageJob job{};
    job.imageIndex = index;
    job.generation = image.generation;
    job.backgroundColor = image.backgroundColor;
    job.useSessionCache = _usingSessionPage;
    image.url.toCharArray(job.url, sizeof(job.url));
    if (xQueueSend(_imageJobs, &job, 0) != pdTRUE) {
      _nextImageToQueue--;
      return;
    }
    image.state = 1;
    browserLog(String(F("image job queued #")) + String(index + 1));
  }
}

void BrowserScreen::collectImageResults() {
  if (!_imageResults) return;
  bool redraw = false;
  bool updateControls = false;
  ImageResult result{};
  while (xQueueReceive(_imageResults, &result, 0) == pdTRUE) {
    updateControls = true;
    if (result.generation == _imageGeneration && result.imageIndex < _embeddedImageCount) {
      EmbeddedImage &image = _embeddedImages[result.imageIndex];
      if (image.generation == result.generation && image.state == 1 && result.pixels && result.width && result.height) {
        image.pixels = result.pixels;
        image.width = result.width;
        image.height = result.height;
        image.state = 2;
        browserLog(String(F("image preview ready #")) + String(result.imageIndex + 1) + F(" (") +
                   String(result.width) + F("x") + String(result.height) + F(")"));
        redraw = true;
        continue;
      }
      if (image.generation == result.generation && image.state == 1) {
        image.state = 3;
        browserLog(String(F("image preview failed #")) + String(result.imageIndex + 1));
      }
    }
    if (result.pixels) free(result.pixels);
  }
  if (redraw && !_deferCachedPageDraw && !_showStart && _message.length() == 0) {
    updateImageLayout();
    buildPages();
    redrawDocument();
  }
  if (updateControls && !_showStart) drawControls();
}

bool BrowserScreen::cachedPageImagesComplete() const {
  if (_nextImageToQueue < _embeddedImageCount || _activeImageGeneration == _imageGeneration) return false;
  for (uint16_t i = 0; i < _embeddedImageCount; i++) {
    if (_embeddedImages[i].generation == _imageGeneration && _embeddedImages[i].state < 2) return false;
  }
  return true;
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
      if (result.pixels) free(result.pixels);
      continue;
    }

    // Avoid parking an obsolete result (and its heap allocation) forever if
    // the queue is full while the user navigates away.
    while (xQueueSend(screen->_imageResults, &result, pdMS_TO_TICKS(50)) != pdTRUE) {
      if (job.generation != screen->_imageGeneration) {
        if (result.pixels) free(result.pixels);
        result.pixels = nullptr;
        break;
      }
    }
  }
}

void BrowserScreen::buildPages() {
  _pageCount = 0;
  if (_rowCount == 0) return;
  const int16_t bottom = _display.height() - ControlsHeight - 4;
  uint16_t first = 0;
  int16_t y = HeaderHeight + DocumentPadding;
  _pageStarts[_pageCount++] = 0;
  for (uint16_t i = 0; i < _rowCount; i++) {
    int16_t height = rowHeight(_rows[i]);
    if (y + height > bottom && i > first) {
      if (_pageCount >= MaxPages) break;
      first = i;
      _pageStarts[_pageCount++] = i;
      y = HeaderHeight + DocumentPadding;
    }
    y += height;
  }
  if (_page >= _pageCount) _page = 0;
}

uint16_t BrowserScreen::pageEndRow(uint8_t page) const {
  if (page + 1 < _pageCount) return _pageStarts[page + 1];
  return _rowCount;
}

bool BrowserScreen::hasNextPage() const {
  return _page + 1 < _pageCount;
}

int16_t BrowserScreen::textWidth(const String &text, const TextStyle &style) const {
  applyFont(style);
  return _display.tft().textWidth(text);
}

void BrowserScreen::applyFont(const TextStyle &style) const {
  TFT_eSPI &tft = _display.tft();
  if (style.bold && style.italic) tft.setFreeFont(&FreeSansBoldOblique9pt7b);
  else if (style.bold) tft.setFreeFont(&FreeSansBold9pt7b);
  else if (style.italic) tft.setFreeFont(&FreeSansOblique9pt7b);
  else tft.setFreeFont(&FreeSans9pt7b);
}

bool BrowserScreen::stylesEqual(const TextStyle &left, const TextStyle &right) const {
  return left.color == right.color && left.backgroundColor == right.backgroundColor &&
         left.hasBackground == right.hasBackground && left.blockBackgroundColor == right.blockBackgroundColor &&
         left.hasBlockBackground == right.hasBlockBackground && left.bold == right.bold && left.italic == right.italic &&
         left.underline == right.underline && left.link == right.link && left.linkIndex == right.linkIndex &&
         left.fullWidthImage == right.fullWidthImage;
}

BrowserScreen::TextStyle BrowserScreen::styledForTag(const String &tag, const String &attributes) {
  TextStyle style = _currentStyle;
  const bool isBlock = isBlockTag(tag);
  if (tag == "img") style.fullWidthImage = false;
  if (tag == "b" || tag == "strong") style.bold = true;
  if (tag == "i" || tag == "em") style.italic = true;
  if (tag == "a") {
    style.underline = true;
    style.link = true;
    style.linkIndex = 0;
    String href = decodeEntities(attributeValue(attributes, "href"));
    href.trim();
    if (href.length() && !href.startsWith("#")) {
      Url base;
      String resolved;
      String error;
      if (parseUrl(_currentUrl, base)) resolved = resolveUrl(base, href);
      String normalized;
      bool isHttp = startsWithIgnoreCase(resolved, "http://") || startsWithIgnoreCase(resolved, "https://");
      if (isHttp && normalizeUrl(resolved, normalized, error)) style.linkIndex = registerLink(normalized);
      else style.linkIndex = 0;
    }
  }

  String color = attributeValue(attributes, "color");
  if (color.length()) style.color = parseColor(color, style.color);

  String background = attributeValue(attributes, "bgcolor");
  if (background.length()) applyBackground(style, background);

  if (tag == "img" && lower(trimCopy(attributeValue(attributes, "width"))) == "100%") {
    style.fullWidthImage = true;
  }

  applyCssRules(style, tag, attributes);

  String inlineStyle = attributeValue(attributes, "style");
  StyleOverride inlineOverride = {};
  parseCssDeclarations(inlineStyle, inlineOverride);
  applyStyleOverride(style, inlineOverride, tag == "img");
  if (isBlock) {
    if (style.hasBackground) {
      style.blockBackgroundColor = style.backgroundColor;
      style.hasBlockBackground = true;
    }
  }
  return style;
}

void BrowserScreen::collectCssRules(const String &html) {
  bool inStyle = false;
  bool inScript = false;
  uint32_t styleStart = 0;
  uint32_t position = 0;
  while (position < html.length() && _cssRuleCount < MaxCssRules) {
    int nextTag = html.indexOf('<', position);
    if (nextTag < 0) break;
    int endTag = findTagEnd(html, nextTag + 1);
    if (endTag < 0) break;
    String inside = trimCopy(html.substring(nextTag + 1, endTag));
    bool closing = inside.startsWith("/");
    if (closing) inside.remove(0, 1);
    inside.trim();
    int space = inside.indexOf(' ');
    String tag = lower(space < 0 ? inside : inside.substring(0, space));
    if (tag.endsWith("/")) tag.remove(tag.length() - 1);
    if (tag == "script") {
      inScript = !closing;
    } else if (tag == "style" && !inScript) {
      if (!closing && !inStyle) {
        inStyle = true;
        styleStart = endTag + 1;
      } else if (closing && inStyle) {
        parseCssRules(html, styleStart, nextTag);
        inStyle = false;
      }
    }
    position = endTag + 1;
  }
}

void BrowserScreen::skipCssSpaceAndComments(const String &source, uint32_t &position, uint32_t end) const {
  while (position < end) {
    if (isspace((unsigned char)source[position])) {
      position++;
      continue;
    }
    if (position + 1 < end && source[position] == '/' && source[position + 1] == '*') {
      position += 2;
      while (position + 1 < end && !(source[position] == '*' && source[position + 1] == '/')) position++;
      position = position + 1 < end ? position + 2 : end;
      continue;
    }
    break;
  }
}

bool BrowserScreen::cssSlice(const String &source, uint32_t start, uint32_t end, uint16_t limit,
                             String &result) const {
  result = "";
  for (uint32_t position = start; position < end; position++) {
    if (position + 1 < end && source[position] == '/' && source[position + 1] == '*') {
      position += 2;
      while (position + 1 < end && !(source[position] == '*' && source[position + 1] == '/')) position++;
      if (position + 1 < end) position++;
      continue;
    }
    if (result.length() >= limit) return false;
    result += source[position];
  }
  result.trim();
  return true;
}

void BrowserScreen::parseCssRules(const String &source, uint32_t start, uint32_t end) {
  uint32_t position = start;
  while (position < end && _cssRuleCount < MaxCssRules) {
    skipCssSpaceAndComments(source, position, end);
    const uint32_t selectorStart = position;
    while (position < end) {
      if (position + 1 < end && source[position] == '/' && source[position + 1] == '*') {
        position += 2;
        while (position + 1 < end && !(source[position] == '*' && source[position + 1] == '/')) position++;
        position = position + 1 < end ? position + 2 : end;
        continue;
      }
      if (source[position] == '{' || source[position] == '}') break;
      position++;
    }
    if (position >= end) break;
    if (source[position] != '{') {
      position++;
      continue;
    }
    const uint32_t selectorEnd = position++;
    const uint32_t declarationsStart = position;
    while (position < end) {
      if (position + 1 < end && source[position] == '/' && source[position + 1] == '*') {
        position += 2;
        while (position + 1 < end && !(source[position] == '*' && source[position + 1] == '/')) position++;
        position = position + 1 < end ? position + 2 : end;
        continue;
      }
      if (source[position] == '}') break;
      position++;
    }
    if (position >= end) break;
    const uint32_t declarationsEnd = position++;
    String selectors;
    String declarations;
    if (!cssSlice(source, selectorStart, selectorEnd, MaxCssSelectorLength, selectors) ||
        !cssSlice(source, declarationsStart, declarationsEnd, MaxCssDeclarationLength, declarations)) continue;
    StyleOverride style = {};
    if (!parseCssDeclarations(declarations, style)) continue;
    const uint16_t sourceOrder = _cssSourceOrder++;
    int selectorStartIndex = 0;
    while (selectorStartIndex < (int)selectors.length() && _cssRuleCount < MaxCssRules) {
      int selectorEndIndex = selectors.indexOf(',', selectorStartIndex);
      String selector = trimCopy(selectors.substring(selectorStartIndex,
        selectorEndIndex < 0 ? selectors.length() : selectorEndIndex));
      addCssRule(selector, style, sourceOrder);
      if (selectorEndIndex < 0) break;
      selectorStartIndex = selectorEndIndex + 1;
    }
  }
}

bool BrowserScreen::addCssRule(const String &rawSelector, const StyleOverride &style, uint16_t sourceOrder) {
  if (_cssRuleCount >= MaxCssRules) return false;
  String selector = trimCopy(rawSelector);
  if (!selector.length()) return false;
  uint8_t selectorType = 0;
  uint8_t specificity = 1;
  if (selector[0] == '.') {
    selectorType = 1;
    specificity = 10;
    selector.remove(0, 1);
  } else if (selector[0] == '#') {
    selectorType = 2;
    specificity = 100;
    selector.remove(0, 1);
  } else {
    selector = lower(selector);
  }
  if (!selector.length() || selector.length() > MaxCssSelectorLength) return false;
  for (uint16_t index = 0; index < selector.length(); index++) {
    char value = selector[index];
    if (!isalnum((unsigned char)value) && value != '-' && value != '_') return false;
  }
  _cssRules[_cssRuleCount] = {selector, style, sourceOrder, specificity, selectorType};
  _cssRuleCount++;
  return true;
}

bool BrowserScreen::parseCssDeclarations(const String &declarations, StyleOverride &style) const {
  bool found = false;
  int start = 0;
  while (start < (int)declarations.length()) {
    int end = declarations.indexOf(';', start);
    String declaration = trimCopy(declarations.substring(start, end < 0 ? declarations.length() : end));
    int colon = declaration.indexOf(':');
    if (colon > 0) {
      String property = lower(trimCopy(declaration.substring(0, colon)));
      String value = lower(trimCopy(declaration.substring(colon + 1)));
      value.replace("!important", "");
      value.trim();
      if (property == "color") {
        uint16_t color;
        if (parseCssColor(value, color)) {
          style.hasColor = true;
          style.color = color;
          found = true;
        }
      } else if (property == "background" || property == "background-color") {
        uint16_t color;
        bool enabled;
        if (parseBackground(value, color, enabled)) {
          style.hasBackground = true;
          style.backgroundColor = color;
          style.backgroundEnabled = enabled;
          found = true;
        }
      } else if (property == "font-weight") {
        bool digits = value.length() > 0;
        for (uint16_t index = 0; index < value.length(); index++) {
          if (!isdigit((unsigned char)value[index])) digits = false;
        }
        if (value == "normal" || value.indexOf("bold") >= 0 || digits) {
          style.hasBold = true;
          style.bold = value.indexOf("bold") >= 0 || (digits && value.toInt() >= 600);
          found = true;
        }
      } else if (property == "font-style") {
        if (value == "normal" || value.indexOf("italic") >= 0 || value.indexOf("oblique") >= 0) {
          style.hasItalic = true;
          style.italic = value.indexOf("italic") >= 0 || value.indexOf("oblique") >= 0;
          found = true;
        }
      } else if (property == "text-decoration") {
        if (value.length()) {
          style.hasUnderline = true;
          style.underline = value.indexOf("underline") >= 0;
          found = true;
        }
      } else if (property == "display" && value == "block") {
        style.hasFullWidthImage = true;
        style.fullWidthImage = true;
        found = true;
      } else if (property == "width" && value == "100%") {
        style.hasFullWidthImage = true;
        style.fullWidthImage = true;
        found = true;
      }
    }
    if (end < 0) break;
    start = end + 1;
  }
  return found;
}

bool BrowserScreen::parseBackground(const String &rawValue, uint16_t &color, bool &enabled) const {
  String value = lower(trimCopy(rawValue));
  value.replace("!important", "");
  value.trim();
  if (value == "transparent") {
    enabled = false;
    return true;
  }
  if (parseCssColor(value, color)) {
    enabled = true;
    return true;
  }
  int start = 0;
  while (start < (int)value.length()) {
    while (start < (int)value.length() && isspace((unsigned char)value[start])) start++;
    int end = start;
    while (end < (int)value.length() && !isspace((unsigned char)value[end])) end++;
    if (parseCssColor(value.substring(start, end), color)) {
      enabled = true;
      return true;
    }
    start = end + 1;
  }
  return false;
}

bool BrowserScreen::matchesCssRule(const CssRule &rule, const String &tag, const String &attributes) const {
  if (rule.selectorType == 0) return rule.selector == tag;
  String value = attributeValue(attributes, rule.selectorType == 1 ? "class" : "id");
  if (rule.selectorType == 2) return value == rule.selector;
  int start = 0;
  while (start < (int)value.length()) {
    while (start < (int)value.length() && isspace((unsigned char)value[start])) start++;
    int end = start;
    while (end < (int)value.length() && !isspace((unsigned char)value[end])) end++;
    if (value.substring(start, end) == rule.selector) return true;
    start = end + 1;
  }
  return false;
}

void BrowserScreen::applyStyleOverride(TextStyle &style, const StyleOverride &override, bool imageTag) const {
  if (override.hasColor) style.color = override.color;
  if (override.hasBackground) {
    style.hasBackground = override.backgroundEnabled;
    if (override.backgroundEnabled) style.backgroundColor = override.backgroundColor;
  }
  if (override.hasBold) style.bold = override.bold;
  if (override.hasItalic) style.italic = override.italic;
  if (override.hasUnderline) style.underline = override.underline;
  if (imageTag && override.hasFullWidthImage) style.fullWidthImage = override.fullWidthImage;
}

void BrowserScreen::applyCssRules(TextStyle &style, const String &tag, const String &attributes) const {
  StyleOverride resolved = {};
  int16_t colorSpecificity = -1, backgroundSpecificity = -1, boldSpecificity = -1;
  int16_t italicSpecificity = -1, underlineSpecificity = -1, imageSpecificity = -1;
  uint16_t colorOrder = 0, backgroundOrder = 0, boldOrder = 0;
  uint16_t italicOrder = 0, underlineOrder = 0, imageOrder = 0;
  for (uint8_t index = 0; index < _cssRuleCount; index++) {
    const CssRule &rule = _cssRules[index];
    if (!matchesCssRule(rule, tag, attributes)) continue;
    const StyleOverride &candidate = rule.style;
    if (candidate.hasColor && (rule.specificity > colorSpecificity ||
        (rule.specificity == colorSpecificity && rule.sourceOrder >= colorOrder))) {
      resolved.hasColor = true; resolved.color = candidate.color;
      colorSpecificity = rule.specificity; colorOrder = rule.sourceOrder;
    }
    if (candidate.hasBackground && (rule.specificity > backgroundSpecificity ||
        (rule.specificity == backgroundSpecificity && rule.sourceOrder >= backgroundOrder))) {
      resolved.hasBackground = true; resolved.backgroundEnabled = candidate.backgroundEnabled;
      resolved.backgroundColor = candidate.backgroundColor;
      backgroundSpecificity = rule.specificity; backgroundOrder = rule.sourceOrder;
    }
    if (candidate.hasBold && (rule.specificity > boldSpecificity ||
        (rule.specificity == boldSpecificity && rule.sourceOrder >= boldOrder))) {
      resolved.hasBold = true; resolved.bold = candidate.bold;
      boldSpecificity = rule.specificity; boldOrder = rule.sourceOrder;
    }
    if (candidate.hasItalic && (rule.specificity > italicSpecificity ||
        (rule.specificity == italicSpecificity && rule.sourceOrder >= italicOrder))) {
      resolved.hasItalic = true; resolved.italic = candidate.italic;
      italicSpecificity = rule.specificity; italicOrder = rule.sourceOrder;
    }
    if (candidate.hasUnderline && (rule.specificity > underlineSpecificity ||
        (rule.specificity == underlineSpecificity && rule.sourceOrder >= underlineOrder))) {
      resolved.hasUnderline = true; resolved.underline = candidate.underline;
      underlineSpecificity = rule.specificity; underlineOrder = rule.sourceOrder;
    }
    if (candidate.hasFullWidthImage && (rule.specificity > imageSpecificity ||
        (rule.specificity == imageSpecificity && rule.sourceOrder >= imageOrder))) {
      resolved.hasFullWidthImage = true; resolved.fullWidthImage = candidate.fullWidthImage;
      imageSpecificity = rule.specificity; imageOrder = rule.sourceOrder;
    }
  }
  applyStyleOverride(style, resolved, tag == "img");
}

uint8_t BrowserScreen::registerLink(const String &url) {
  for (uint8_t index = 0; index < _linkCount; index++) {
    if (_linkUrls[index] == url) return index + 1;
  }
  if (_linkCount >= MaxLinks) return 0;
  _linkUrls[_linkCount] = url;
  _linkCount++;
  return _linkCount;
}

String BrowserScreen::attributeValue(const String &attributes, const char *name) const {
  String wanted = lower(String(name));
  uint16_t index = 0;
  while (index < attributes.length()) {
    while (index < attributes.length() && (isspace((unsigned char)attributes[index]) || attributes[index] == '/')) index++;
    uint16_t nameStart = index;
    while (index < attributes.length() && !isspace((unsigned char)attributes[index]) &&
           attributes[index] != '=' && attributes[index] != '/') index++;
    if (nameStart == index) {
      index++;
      continue;
    }
    String attributeName = lower(attributes.substring(nameStart, index));
    while (index < attributes.length() && isspace((unsigned char)attributes[index])) index++;
    if (index >= attributes.length() || attributes[index] != '=') {
      continue;
    }
    index++;
    while (index < attributes.length() && isspace((unsigned char)attributes[index])) index++;
    String value;
    if (index < attributes.length() && (attributes[index] == '\'' || attributes[index] == '\"')) {
      char quote = attributes[index++];
      uint16_t valueStart = index;
      while (index < attributes.length() && attributes[index] != quote) index++;
      value = attributes.substring(valueStart, index);
      if (index < attributes.length()) index++;
    } else {
      uint16_t valueStart = index;
      while (index < attributes.length() && !isspace((unsigned char)attributes[index])) index++;
      value = attributes.substring(valueStart, index);
    }
    if (attributeName == wanted) return value;
  }
  return "";
}

uint16_t BrowserScreen::parseColor(const String &rawValue, uint16_t fallback) const {
  String value = lower(trimCopy(rawValue));
  if (value.startsWith("#")) {
    String digits = value.substring(1);
    for (uint16_t i = 0; i < digits.length(); i++) {
      if (!isxdigit((unsigned char)digits[i])) return fallback;
    }
    char *end = nullptr;
    if (digits.length() == 3) {
      uint8_t red = strtoul(digits.substring(0, 1).c_str(), &end, 16) * 17;
      uint8_t green = strtoul(digits.substring(1, 2).c_str(), &end, 16) * 17;
      uint8_t blue = strtoul(digits.substring(2, 3).c_str(), &end, 16) * 17;
      return ((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3);
    }
    if (digits.length() == 6) {
      uint8_t red = strtoul(digits.substring(0, 2).c_str(), &end, 16);
      uint8_t green = strtoul(digits.substring(2, 4).c_str(), &end, 16);
      uint8_t blue = strtoul(digits.substring(4, 6).c_str(), &end, 16);
      return ((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3);
    }
  }
  if (value == "black") return TFT_BLACK;
  if (value == "white") return TFT_WHITE;
  if (value == "red") return TFT_RED;
  if (value == "green") return TFT_GREEN;
  if (value == "blue") return TFT_BLUE;
  if (value == "yellow") return TFT_YELLOW;
  if (value == "cyan" || value == "aqua") return TFT_CYAN;
  if (value == "magenta" || value == "fuchsia") return TFT_MAGENTA;
  if (value == "gray" || value == "grey") return TFT_LIGHTGREY;
  if (value == "darkgray" || value == "darkgrey") return TFT_DARKGREY;
  if (value == "orange") return TFT_ORANGE;
  if (value == "purple") return 0x8010;
  return fallback;
}

bool BrowserScreen::parseCssColor(const String &rawValue, uint16_t &color) const {
  String value = lower(trimCopy(rawValue));
  if (value.startsWith("#")) {
    String digits = value.substring(1);
    if (digits.length() != 3 && digits.length() != 6) return false;
    for (uint16_t i = 0; i < digits.length(); i++) {
      if (!isxdigit((unsigned char)digits[i])) return false;
    }
    color = parseColor(value, TFT_BLACK);
    return true;
  }
  if (value.startsWith("rgb(") && value.endsWith(")")) {
    int red = -1;
    int green = -1;
    int blue = -1;
    if (sscanf(value.c_str(), "rgb(%d,%d,%d)", &red, &green, &blue) == 3 &&
        red >= 0 && red <= 255 && green >= 0 && green <= 255 && blue >= 0 && blue <= 255) {
      color = ((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3);
      return true;
    }
  }
  if (value == "black" || value == "white" || value == "red" || value == "green" ||
      value == "blue" || value == "yellow" || value == "cyan" || value == "aqua" ||
      value == "magenta" || value == "fuchsia" || value == "gray" || value == "grey" ||
      value == "darkgray" || value == "darkgrey" || value == "orange" || value == "purple") {
    color = parseColor(value, TFT_BLACK);
    return true;
  }
  return false;
}

void BrowserScreen::applyBackground(TextStyle &style, const String &rawValue) const {
  uint16_t color = TFT_BLACK;
  bool enabled = false;
  if (!parseBackground(rawValue, color, enabled)) return;
  style.hasBackground = enabled;
  if (enabled) style.backgroundColor = color;
}

int BrowserScreen::findTagEnd(const String &html, int start) const {
  char quote = '\0';
  for (int index = start; index < (int)html.length(); index++) {
    char value = html[index];
    if (quote) {
      if (value == quote) quote = '\0';
    } else if (value == '\'' || value == '\"') {
      quote = value;
    } else if (value == '>') {
      return index;
    }
  }
  return -1;
}

void BrowserScreen::pushStyle(const String &tag, const TextStyle &style) {
  if (_styleDepth >= MaxStyleDepth) return;
  _styleStack[_styleDepth].tag = tag;
  _styleStack[_styleDepth].style = style;
  _styleDepth++;
  _currentStyle = style;
}

void BrowserScreen::popStyle(const String &tag) {
  for (int8_t index = _styleDepth - 1; index >= 0; index--) {
    if (_styleStack[index].tag != tag) continue;
    _styleDepth = index;
    _currentStyle = index > 0 ? _styleStack[index - 1].style :
      TextStyle{DefaultTextColor, TFT_WHITE, false, TFT_WHITE, false, false, false, false, false, 0, false};
    return;
  }
}

String BrowserScreen::decodeEntities(const String &value) const {
  String result = value;
  result.replace("&nbsp;", " ");
  result.replace("&amp;", "&");
  result.replace("&lt;", "<");
  result.replace("&gt;", ">");
  result.replace("&quot;", "\"");
  result.replace("&#39;", "'");
  result.replace("&apos;", "'");
  String decoded;
  for (uint32_t i = 0; i < result.length();) {
    if (result[i] == '&' && i + 3 < result.length() && result[i + 1] == '#') {
      int end = result.indexOf(';', i + 2);
      if (end > (int)i + 2) {
        String number = result.substring(i + 2, end);
        bool hexadecimal = number.startsWith("x") || number.startsWith("X");
        if (hexadecimal) number.remove(0, 1);
        char *numberEnd = nullptr;
        uint32_t codepoint = strtoul(number.c_str(), &numberEnd, hexadecimal ? 16 : 10);
        if (number.length() && numberEnd && *numberEnd == '\0' && codepoint <= 0x10ffffUL) {
          if (codepoint < 0x80) decoded += (char)codepoint;
          else if (codepoint < 0x800) {
            decoded += (char)(0xc0 | (codepoint >> 6));
            decoded += (char)(0x80 | (codepoint & 0x3f));
          } else if (codepoint < 0x10000) {
            decoded += (char)(0xe0 | (codepoint >> 12));
            decoded += (char)(0x80 | ((codepoint >> 6) & 0x3f));
            decoded += (char)(0x80 | (codepoint & 0x3f));
          }
          i = end + 1;
          continue;
        }
      }
    }
    decoded += result[i++];
  }
  return transliterateCyrillic(decoded);
}

String BrowserScreen::transliterateCyrillic(const String &value) const {
  // UTF-8 Cyrillic is split across several byte ranges. In particular,
  // lowercase а-п are D0 B0..BF; omitting that range was the source of the
  // question marks seen on Russian pages.
  static const char *ExtendedUpper[] = {
    "E", "Yo", "Dj", "Gj", "Ye", "Dz", "I", "Yi", "J", "Lj", "Nj", "C", "K", "I", "U", "Dzh"
  };
  static const char *Upper[] = {
    "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",
    "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "\"", "Y", "'", "E", "Yu", "Ya"
  };
  // а-п: U+0430..U+043F -> D0 B0..BF
  static const char *RussianLowerAtoP[] = {
    "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p"
  };
  // р-я: U+0440..U+044F -> D1 80..8F
  static const char *RussianLowerRtoYa[] = {
    "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "\"", "y", "'", "e", "yu", "ya"
  };
  static const char *ExtendedLower[] = {
    "e", "yo", "dj", "gj", "ye", "dz", "i", "yi", "j", "lj", "nj", "c", "k", "i", "u", "dzh"
  };
  String result;
  for (uint16_t i = 0; i < value.length(); i++) {
    uint8_t first = (uint8_t)value[i];
    if (first < 0x80) {
      result += (char)first;
      continue;
    }
    if (i + 1 >= value.length()) {
      result += '?';
      break;
    }
    uint8_t second = (uint8_t)value[++i];
    if (first == 0xd0 && second >= 0x80 && second <= 0x8f) result += ExtendedUpper[second - 0x80];
    else if (first == 0xd0 && second >= 0x90 && second <= 0xaf) result += Upper[second - 0x90];
    else if (first == 0xd0 && second >= 0xb0 && second <= 0xbf) result += RussianLowerAtoP[second - 0xb0];
    else if (first == 0xd1 && second >= 0x80 && second <= 0x8f) result += RussianLowerRtoYa[second - 0x80];
    else if (first == 0xd1 && second >= 0x90 && second <= 0x9f) result += ExtendedLower[second - 0x90];
    else if (first == 0xd2 && second == 0x90) result += "G";
    else if (first == 0xd2 && second == 0x91) result += "g";
    else result += '?';
  }
  return result;
}

String BrowserScreen::shortText(const String &value, int16_t maxWidth) const {
  if (_display.tft().textWidth(value, 2) <= maxWidth) return value;
  String result = value;
  while (result.length() > 3 && _display.tft().textWidth(result + "...", 2) > maxWidth) result.remove(result.length() - 1);
  return result + "...";
}

String BrowserScreen::shortImageLabel(const String &value, int16_t maxWidth) const {
  TextStyle style{DefaultTextColor, TFT_WHITE, false, TFT_WHITE, false, false, false, false, false, 0, false};
  if (textWidth(value, style) <= maxWidth) return value;
  String result = value;
  while (result.length() > 3 && textWidth(result + "...", style) > maxWidth) {
    result.remove(result.length() - 1);
  }
  return result.length() == value.length() ? result : result + "...";
}

String BrowserScreen::lower(const String &value) const {
  String result = value;
  result.toLowerCase();
  return result;
}

bool BrowserScreen::isBlockTag(const String &tag) const {
  return tag == "body" || tag == "p" || tag == "div" || tag == "section" || tag == "article" || tag == "header" ||
         tag == "footer" || tag == "main" || tag == "nav" || tag == "h1" || tag == "h2" || tag == "h3" ||
         tag == "h4" || tag == "h5" || tag == "h6" || tag == "ul" || tag == "ol" || tag == "li" ||
         tag == "table" || tag == "tr" || tag == "td" || tag == "th" || tag == "blockquote" || tag == "hr";
}

int16_t BrowserScreen::imageWidth(const String &label) const {
  TextStyle style{DefaultTextColor, TFT_WHITE, false, TFT_WHITE, false, false, false, false, false, 0, false};
  return 26 + textWidth(label, style);
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
  uint8_t found = _recentCount;
  for (uint8_t i = 0; i < _recentCount; i++) {
    if (_recentSites[i].url == url) {
      found = i;
      break;
    }
  }
  if (found < _recentCount) {
    for (uint8_t i = found; i > 0; i--) _recentSites[i] = _recentSites[i - 1];
  } else {
    if (_recentCount < MaxRecentSites) _recentCount++;
    for (uint8_t i = _recentCount - 1; i > 0; i--) _recentSites[i] = _recentSites[i - 1];
  }
  _recentSites[0] = {url, title};
  browserLog(String(F("recent site saved: entries=")) + String(_recentCount));
}

void BrowserScreen::loadState() {
  _recentCount = 0;
  _cookieCount = 0;
  if (!_sdCard.isMounted()) {
    browserLog(F("saved state skipped: SD card is not mounted"));
    return;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    browserLog(F("saved state skipped: SD card bus busy"));
    return;
  }
  File file = SD.open(StatePath, FILE_READ);
  if (!file) {
    SpiBusLock::unlock();
    browserLog(F("no saved browser state found"));
    return;
  }
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() < 2 || line[1] != '\t') continue;
    String fields[7];
    uint8_t fieldCount = 0;
    int start = 2;
    while (fieldCount < 7 && start <= (int)line.length()) {
      int end = line.indexOf('\t', start);
      fields[fieldCount++] = line.substring(start, end < 0 ? line.length() : end);
      if (end < 0) break;
      start = end + 1;
    }
    if (line[0] == 'R' && fieldCount >= 2 && _recentCount < MaxRecentSites) {
      _recentSites[_recentCount++] = {decodeField(fields[0]), decodeField(fields[1])};
    } else if (line[0] == 'C' && fieldCount >= 6 && _cookieCount < MaxCookies) {
      Cookie &cookie = _cookies[_cookieCount++];
      cookie.name = decodeField(fields[0]);
      cookie.value = decodeField(fields[1]);
      cookie.domain = decodeField(fields[2]);
      cookie.path = decodeField(fields[3]);
      cookie.secure = fields[4] == "1";
      cookie.expiresAt = strtoull(fields[5].c_str(), nullptr, 10);
    }
  }
  file.close();
  SpiBusLock::unlock();
  pruneCookies();
  browserLog(String(F("saved state loaded: recent=")) + String(_recentCount) +
             F(", cookies=") + String(_cookieCount));
}

void BrowserScreen::saveState() {
  pruneCookies();
  if (!_sdCard.isMounted()) {
    browserLog(F("saved state not written: SD card is not mounted"));
    return;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    browserLog(F("saved state not written: SD card bus busy"));
    return;
  }
  if (!SD.exists("/.system") && !SD.mkdir("/.system")) {
    SpiBusLock::unlock();
    browserLog(F("saved state not written: system directory unavailable"));
    return;
  }
  SD.remove(StateTempPath);
  File file = SD.open(StateTempPath, FILE_WRITE);
  if (!file) {
    SpiBusLock::unlock();
    browserLog(F("saved state not written: temporary file open failed"));
    return;
  }
  bool written = true;
  for (uint8_t i = 0; i < _recentCount; i++) {
    written = written && file.printf("R\t%s\t%s\n", encodeField(_recentSites[i].url).c_str(),
                                     encodeField(_recentSites[i].title).c_str()) > 0;
  }
  for (uint8_t i = 0; i < _cookieCount; i++) {
    const Cookie &cookie = _cookies[i];
    written = written && file.printf("C\t%s\t%s\t%s\t%s\t%d\t%llu\n",
      encodeField(cookie.name).c_str(), encodeField(cookie.value).c_str(),
      encodeField(cookie.domain).c_str(), encodeField(cookie.path).c_str(), cookie.secure ? 1 : 0,
      (unsigned long long)cookie.expiresAt) > 0;
  }
  file.close();
  if (written) {
    if (SD.exists(StatePath)) SD.remove(StatePath);
    if (!SD.rename(StateTempPath, StatePath)) SD.remove(StateTempPath);
  } else {
    SD.remove(StateTempPath);
  }
  SpiBusLock::unlock();
  browserLog(written ? F("saved browser state committed") : F("saved browser state write failed"));
}

String BrowserScreen::encodeField(const String &value) const {
  const char hex[] = "0123456789ABCDEF";
  String result;
  for (uint16_t i = 0; i < value.length(); i++) {
    uint8_t valueByte = (uint8_t)value[i];
    if (isalnum(valueByte) || valueByte == '-' || valueByte == '_' || valueByte == '.' || valueByte == '/') result += (char)valueByte;
    else {
      result += '%';
      result += hex[valueByte >> 4];
      result += hex[valueByte & 15];
    }
  }
  return result;
}

String BrowserScreen::decodeField(const String &value) const {
  String result;
  for (uint16_t i = 0; i < value.length(); i++) {
    if (value[i] == '%' && i + 2 < value.length()) {
      char buffer[] = {value[i + 1], value[i + 2], '\0'};
      result += (char)strtoul(buffer, nullptr, 16);
      i += 2;
    } else {
      result += value[i];
    }
  }
  return result;
}
