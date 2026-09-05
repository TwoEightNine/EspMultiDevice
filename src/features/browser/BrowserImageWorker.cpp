#include "BrowserImageWorker.h"

#include <FS.h>
#include <SD.h>
#include <new>
#include <stdlib.h>
#include <time.h>
#include <type_traits>
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>

// JPEGDEC's filesystem overload refers to File without its fs namespace.
using File = fs::File;
#include <JPEGDEC.h>
#include <PNGdec.h>
#include "../../../third_party/AnimatedGIF/AnimatedGIF.h"

#include "BrowserPersistentState.h"
#include "BrowserSessionCache.h"
#include "../../core/SpiBusLock.h"

static_assert(std::is_trivially_copyable<Browser::ImageJob>::value,
              "Browser image jobs must be safe for FreeRTOS queue copies");
static_assert(std::is_trivially_copyable<Browser::ImageResult>::value,
              "Browser image results must be safe for FreeRTOS queue copies");

namespace {
constexpr uint8_t ThumbnailMaxWidth = 200;
constexpr uint8_t ThumbnailMaxHeight = 200;

void imageLog(const __FlashStringHelper *message) {
  Serial.print(F("[Browser images] "));
  Serial.println(message);
}

void imageLog(const String &message) {
  Serial.print(F("[Browser images] "));
  Serial.println(message);
}

void freeImagePixels(uint16_t *pixels, const __FlashStringHelper *owner) {
  if (!pixels) return;
  if (!heap_caps_check_integrity_addr(reinterpret_cast<intptr_t>(pixels), false)) {
    imageLog(String(F("pixel buffer was not freed: invalid pointer in ")) + owner);
    return;
  }
  free(pixels);
}

// Decoder callbacks are used only by the single Browser image task. The
// decoder writes a bounded RGB565 surface instead of a source-sized canvas.
class ThumbnailDecoder {
public:
  ~ThumbnailDecoder() { discard(); }

  bool decode(const String &path, FileFormats::Format format, uint16_t *&pixels,
              uint8_t &width, uint8_t &height, uint16_t backgroundColor) {
    pixels = nullptr;
    width = height = 0;
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
    const size_t byteCount = static_cast<size_t>(scaledWidth) * scaledHeight * sizeof(uint16_t);
    _pixels = static_cast<uint16_t *>(psramFound() ? ps_malloc(byteCount) : malloc(byteCount));
    if (!_pixels) return false;
    for (uint32_t index = 0; index < scaledWidth * scaledHeight; ++index) {
      _pixels[index] = _backgroundColor;
    }
    _width = static_cast<uint8_t>(scaledWidth);
    _height = static_cast<uint8_t>(scaledHeight);
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
    freeImagePixels(_pixels, F("thumbnail decoder"));
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
    const int options = scale == 8 ? JPEG_SCALE_EIGHTH : scale == 4 ? JPEG_SCALE_QUARTER :
                        scale == 2 ? JPEG_SCALE_HALF : 0;
    const bool ok = _jpeg.decode(0, 0, options) == 1;
    _jpeg.close();
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

  bool decodePng(const String &path, uint16_t *&pixels, uint8_t &width, uint8_t &height) {
    _owner = this;
    if (_png.open(path.c_str(), pngOpen, pngClose, pngRead, pngSeek, pngDraw) != PNG_SUCCESS ||
        _png.isInterlaced()) {
      _png.close();
      _owner = nullptr;
      discard();
      return false;
    }
    if (!prepare(static_cast<uint16_t>(_png.getWidth()), static_cast<uint16_t>(_png.getHeight()),
                 static_cast<uint16_t>(_png.getWidth()), static_cast<uint16_t>(_png.getHeight()),
                 pixels, width, height)) {
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
    const uint8_t x = static_cast<uint32_t>(sourceX) * _width / sourceWidth;
    const uint8_t y = static_cast<uint32_t>(sourceY) * _height / sourceHeight;
    if (x < _width && y < _height) _pixels[static_cast<uint16_t>(y) * _width + x] = color;
  }

  static int jpegDraw(JPEGDRAW *draw) {
    if (!_owner || !draw || !draw->pPixels) return 0;
    for (int y = 0; y < draw->iHeight; ++y) {
      for (int x = 0; x < draw->iWidthUsed; ++x) {
        _owner->store(draw->x + x, draw->y + y,
                      draw->pPixels[y * draw->iWidth + x], true);
      }
    }
    return 1;
  }

  static void *pngOpen(const char *name, int32_t *size) {
    if (!_owner) return nullptr;
    _owner->_file = SD.open(name, FILE_READ);
    if (!_owner->_file) return nullptr;
    *size = static_cast<int32_t>(_owner->_file.size());
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
    const size_t lineBytes = (static_cast<size_t>(draw->iWidth) + 7) * sizeof(uint16_t);
    uint16_t *line = static_cast<uint16_t *>(psramFound() ? ps_malloc(lineBytes) : malloc(lineBytes));
    if (!line) return 0;
    const uint16_t background = _owner->_backgroundColor;
    const uint32_t backgroundRgb = (static_cast<uint32_t>(background >> 8 & 0xf8)) |
                                   (static_cast<uint32_t>(background >> 3 & 0xfc) << 8) |
                                   (static_cast<uint32_t>(background << 3 & 0xf8) << 16);
    _owner->_png.getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, backgroundRgb);
    for (int x = 0; x < draw->iWidth; ++x) _owner->store(x, draw->y, line[x], false);
    free(line);
    return 1;
  }

  static void *gifOpen(const char *name, int32_t *size) { return pngOpen(name, size); }
  static void gifClose(void *handle) { pngClose(handle); }

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
    for (int x = 0; x < draw->iWidth; ++x) {
      const uint8_t paletteIndex = draw->pPixels[x];
      if (draw->ucHasTransparency && paletteIndex == draw->ucTransparent) continue;
      _owner->store(draw->iX + x, draw->iY + draw->y,
                    draw->pPalette[paletteIndex], false);
    }
  }
};

ThumbnailDecoder *ThumbnailDecoder::_owner = nullptr;
}  // namespace

BrowserImageWorker::BrowserImageWorker(BrowserTransport &transport,
                                       BrowserSessionCache &sessionCache,
                                       BrowserPersistentState &persistentState)
    : _transport(transport),
      _sessionCache(sessionCache),
      _persistentState(persistentState),
      _jobs(xQueueCreate(BrowserLimits::ImageJobQueueLength, sizeof(QueuedJob))),
      _results(xQueueCreate(BrowserLimits::ImageJobQueueLength, sizeof(Browser::ImageResult))),
      _stateMutex(xSemaphoreCreateMutex()),
      _task(nullptr),
      _generation(1),
      _activeGeneration(0) {
  for (auto &image : _completed) image = {};
}

bool BrowserImageWorker::begin() {
  if (_task) return true;
  if (!_jobs || !_results || !_stateMutex) {
    imageLog(F("worker unavailable: queue or state allocation failed"));
    return false;
  }
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  const BaseType_t workerCore = 0;
#else
  const BaseType_t workerCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
  const bool started = xTaskCreatePinnedToCore(taskMain, "browser-images", 16384, this,
                                               tskIDLE_PRIORITY + 1, &_task,
                                               workerCore) == pdPASS;
  if (started) imageLog(String(F("worker started on core ")) + workerCore);
  else imageLog(F("worker task creation failed"));
  return started;
}

bool BrowserImageWorker::enqueue(const Browser::ImageJob &job, const String &userAgent) {
  if (!available() || job.generation != _generation) return false;
  QueuedJob queued{};
  queued.image = job;
  userAgent.toCharArray(queued.userAgent, sizeof(queued.userAgent));
  return xQueueSend(_jobs, &queued, 0) == pdTRUE;
}

bool BrowserImageWorker::take(Browser::ImageResult &result) {
  if (!_results) return false;
  Browser::ImageResult candidate{};
  while (xQueueReceive(_results, &candidate, 0) == pdTRUE) {
    if (candidate.generation == _generation) {
      result = candidate;
      return true;
    }
    releaseCompleted(candidate.imageIndex, candidate.generation, candidate.pixels);
  }
  return false;
}

void BrowserImageWorker::discard(const Browser::ImageResult &result) {
  releaseCompleted(result.imageIndex, result.generation, result.pixels);
}

void BrowserImageWorker::cancel() {
  if (!_stateMutex) {
    ++_generation;
    if (_generation == 0) _generation = 1;
    return;
  }
  uint16_t *released[BrowserLimits::MaxEmbeddedImages] = {};
  uint16_t releaseCount = 0;
  xSemaphoreTake(_stateMutex, portMAX_DELAY);
  ++_generation;
  if (_generation == 0) _generation = 1;
  for (auto &image : _completed) {
    if (image.pixels) released[releaseCount++] = image.pixels;
    image = {};
  }
  xSemaphoreGive(_stateMutex);

  if (_jobs) {
    QueuedJob job{};
    while (xQueueReceive(_jobs, &job, 0) == pdTRUE) {}
  }
  if (_results) {
    Browser::ImageResult result{};
    while (xQueueReceive(_results, &result, 0) == pdTRUE) {}
  }
  for (uint16_t index = 0; index < releaseCount; ++index) {
    freeImagePixels(released[index], F("cancelled generation"));
  }
}

void BrowserImageWorker::taskMain(void *context) {
  BrowserImageWorker *worker = static_cast<BrowserImageWorker *>(context);
  QueuedJob job{};
  while (true) {
    if (xQueueReceive(worker->_jobs, &job, portMAX_DELAY) != pdTRUE) continue;
    worker->run(job);
  }
}

void BrowserImageWorker::run(const QueuedJob &job) {
  if (!activate(job.image.generation)) return;
  Browser::ImageResult result{};
  result.imageIndex = job.image.imageIndex;
  result.generation = job.image.generation;
  imageLog(String(F("started #")) + String(result.imageIndex + 1));
  fetch(String(job.image.url), String(job.userAgent), job.image.backgroundColor,
        job.image.useSessionCache, result.pixels, result.width, result.height);
  deactivate(job.image.generation);
  imageLog(String(F("finished #")) + String(result.imageIndex + 1));

  if (!isCurrent(result.generation)) {
    freeImagePixels(result.pixels, F("cancelled worker result"));
    return;
  }
  if (result.pixels && !retainCompleted(result)) {
    freeImagePixels(result.pixels, F("untracked worker result"));
    result.pixels = nullptr;
    result.width = result.height = 0;
  }
  while (xQueueSend(_results, &result, pdMS_TO_TICKS(50)) != pdTRUE) {
    if (!isCurrent(result.generation)) {
      releaseCompleted(result.imageIndex, result.generation, result.pixels);
      return;
    }
  }
}

bool BrowserImageWorker::activate(uint32_t generation) {
  if (!_stateMutex) return false;
  xSemaphoreTake(_stateMutex, portMAX_DELAY);
  const bool current = generation == _generation;
  if (current) _activeGeneration = generation;
  xSemaphoreGive(_stateMutex);
  return current;
}

void BrowserImageWorker::deactivate(uint32_t generation) {
  if (!_stateMutex) return;
  xSemaphoreTake(_stateMutex, portMAX_DELAY);
  if (_activeGeneration == generation) _activeGeneration = 0;
  xSemaphoreGive(_stateMutex);
}

bool BrowserImageWorker::isCurrent(uint32_t generation) const {
  return generation == _generation;
}

bool BrowserImageWorker::retainCompleted(const Browser::ImageResult &result) {
  if (!_stateMutex || !result.pixels) return false;
  xSemaphoreTake(_stateMutex, portMAX_DELAY);
  bool retained = false;
  if (result.generation == _generation) {
    for (auto &image : _completed) {
      if (image.pixels) continue;
      image = {result.imageIndex, result.generation, result.pixels};
      retained = true;
      break;
    }
  }
  xSemaphoreGive(_stateMutex);
  return retained;
}

void BrowserImageWorker::releaseCompleted(uint16_t imageIndex, uint32_t generation,
                                          uint16_t *pixels) {
  if (!_stateMutex || !pixels) return;
  uint16_t *released = nullptr;
  xSemaphoreTake(_stateMutex, portMAX_DELAY);
  for (auto &image : _completed) {
    if (image.pixels != pixels || image.imageIndex != imageIndex || image.generation != generation) continue;
    released = image.pixels;
    image = {};
    break;
  }
  xSemaphoreGive(_stateMutex);
  freeImagePixels(released, F("discarded result"));
}

bool BrowserImageWorker::fetch(const String &url, const String &userAgent,
                               uint16_t backgroundColor, bool useSessionCache,
                               uint16_t *&pixels, uint8_t &width, uint8_t &height) {
  pixels = nullptr;
  width = height = 0;
  String sourcePath = useSessionCache ? _sessionCache.findImage(url) : "";
  bool cachedSource = sourcePath.length() > 0;
  if (cachedSource) imageLog(String(F("session cache hit: ")) + url);

  if (!cachedSource) {
    Browser::TransferResult transfer = _transport.transfer(
      {Browser::HttpMethod::Get, url, ""}, requestOptions(url, userAgent), *this, *this);
    if (!transfer.succeeded() || !transfer.downloadPath.length()) {
      imageLog(String(F("fetch failed: ")) + transfer.error);
      return false;
    }
    const String downloadedPath = transfer.downloadPath;
    const FileFormats::Format downloadedFormat = FileFormats::fromPath(downloadedPath);
    if (FileFormats::group(downloadedFormat) != FileFormats::Group::Image) {
      if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
        SD.remove(downloadedPath.c_str());
        SpiBusLock::unlock();
      }
      return false;
    }
    sourcePath = _sessionCache.adoptImage(url, downloadedPath, downloadedFormat);
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

  void *decoderStorage = psramFound() ? ps_malloc(sizeof(ThumbnailDecoder))
                                      : malloc(sizeof(ThumbnailDecoder));
  if (!decoderStorage) {
    imageLog(String(F("decoder allocation failed: bytes=")) + String(sizeof(ThumbnailDecoder)) +
             F(", freeHeap=") + String(ESP.getFreeHeap()) +
             F(", freePsram=") + String(ESP.getFreePsram()));
    return false;
  }
  ThumbnailDecoder *decoder = new (decoderStorage) ThumbnailDecoder();
  SpiBusLock::lock();
  const bool decoded = decoder->decode(sourcePath, sourceFormat, pixels, width, height,
                                       backgroundColor);
  SpiBusLock::unlock();
  decoder->~ThumbnailDecoder();
  free(decoderStorage);
  if (!cachedSource && SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    SD.remove(sourcePath.c_str());
    SpiBusLock::unlock();
  }
  if (!decoded || !pixels || !width || !height) {
    if (cachedSource) _sessionCache.discardImage(url);
    freeImagePixels(pixels, F("decode failure"));
    pixels = nullptr;
    width = height = 0;
    return false;
  }
  imageLog(String(F("decoded: ")) + String(width) + F("x") + String(height));
  return true;
}

Browser::RequestOptions BrowserImageWorker::requestOptions(const String &url,
                                                           const String &userAgent) const {
  Browser::Url parsedUrl;
  String cookies;
  if (BrowserTransport::parseUrl(url, parsedUrl)) cookies = cookieHeaderFor(parsedUrl);
  return {userAgent, cookies, BrowserLimits::MaxBodyBytes, BrowserLimits::MaxEmbeddedImageBytes};
}

bool BrowserImageWorker::cancelled() const {
  return _activeGeneration == 0 || _activeGeneration != _generation;
}

void BrowserImageWorker::phaseChanged(const String &phase, int32_t totalBytes) {
  (void)phase;
  (void)totalBytes;
}

void BrowserImageWorker::bytesReceived(uint32_t bytes) { (void)bytes; }

void BrowserImageWorker::cookieReceived(const String &header, const Browser::Url &url) {
  _persistentState.acceptCookie(header, url, nowEpoch());
}

String BrowserImageWorker::cookieHeaderFor(const Browser::Url &url) const {
  return _persistentState.cookieHeaderFor(url, nowEpoch());
}

uint64_t BrowserImageWorker::nowEpoch() {
  const time_t value = time(nullptr);
  return value > 1577836800 ? static_cast<uint64_t>(value) : 0;
}
