#include "FilesScreen.h"

#include "Navigator.h"
#include "SpiBusLock.h"
#include "icons.h"

#include <FS.h>
#include <SD.h>
#include <JPEGDEC.h>
#ifndef PNG_MAX_BUFFERED_PIXELS
#define PNG_MAX_BUFFERED_PIXELS ((4096 * 4 + 1) * 2)
#endif
#include <PNGdec.h>
#include <esp32-hal-psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

#include "third_party/AnimatedGIF/AnimatedGIF.h"
#include "third_party/libwebp/src/webp/decode.h"
#include "third_party/libwebp/src/webp/demux.h"

using File = fs::File;

namespace {
const uint16_t FolderColor = TFT_YELLOW;
const uint16_t FileColor = TFT_LIGHTGREY;
const uint16_t ImageFileColor = TFT_GREEN;
const uint16_t AudioFileColor = TFT_BLUE;
const uint16_t VideoFileColor = TFT_ORANGE;
const uint16_t MenuColor = 0x2104;
const uint16_t DangerColor = TFT_RED;
}

void thumbnailLog(const String &message) {
  Serial.print(F("[Files thumbnails] "));
  Serial.println(message);
}

class FileThumbnailWorker {
public:
  static const uint8_t Size = 32;
  static const uint16_t MaxPath = 256;
  static const uint8_t CacheHeaderBytes = 20;
  struct Result { uint32_t generation; char path[MaxPath]; uint16_t *pixels; };

  FileThumbnailWorker()
    : _jobs(xQueueCreate(2, sizeof(Job))), _results(xQueueCreate(2, sizeof(Result))),
      _task(nullptr), _pixels(nullptr), _sourceWidth(0), _sourceHeight(0),
      _decodedWidth(0), _decodedHeight(0), _sourceSize(0), _sourceWriteTime(0), _gif(nullptr) {
    if (_jobs && _results) {
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
      const BaseType_t core = 0;
#else
      const BaseType_t core = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
      xTaskCreatePinnedToCore(taskMain, "file-thumbs", 12288, this, tskIDLE_PRIORITY + 1, &_task, core);
    }
  }

  bool enqueue(const String &path, FileFormats::Format format, uint32_t generation) {
    if (!_task || path.length() == 0 || path.length() >= MaxPath) return false;
    Job job{};
    job.generation = generation;
    job.format = format;
    path.toCharArray(job.path, sizeof(job.path));
    return xQueueSend(_jobs, &job, 0) == pdTRUE;
  }

  bool take(Result &result) { return _results && xQueueReceive(_results, &result, 0) == pdTRUE; }

private:
  struct Job { uint32_t generation; FileFormats::Format format; char path[MaxPath]; };
  QueueHandle_t _jobs;
  QueueHandle_t _results;
  TaskHandle_t _task;
  File _file;
  JPEGDEC _jpeg;
  PNG _png;
  AnimatedGIF *_gif;
  uint16_t *_pixels;
  uint16_t _sourceWidth;
  uint16_t _sourceHeight;
  uint16_t _decodedWidth;
  uint16_t _decodedHeight;
  uint32_t _sourceSize;
  uint32_t _sourceWriteTime;
  String _failure;
  static FileThumbnailWorker *_owner;

  static void taskMain(void *context) {
    FileThumbnailWorker *worker = static_cast<FileThumbnailWorker *>(context);
    Job job{};
    while (true) {
      if (xQueueReceive(worker->_jobs, &job, portMAX_DELAY) != pdTRUE) continue;
      Result result{};
      result.generation = job.generation;
      strncpy(result.path, job.path, sizeof(result.path) - 1);
      thumbnailLog(String(F("start ")) + job.path);
      result.pixels = worker->decode(String(job.path), job.format);
      thumbnailLog(result.pixels ? String(F("ready ")) + job.path
                                 : String(F("failed ")) + job.path + F(": ") + worker->_failure);
      xQueueSend(worker->_results, &result, portMAX_DELAY);
    }
  }

  void reset() {
    if (_gif) { _gif->close(); delete _gif; _gif = nullptr; }
    closeFile();
    if (_pixels) free(_pixels);
    _pixels = nullptr;
    _sourceWidth = _sourceHeight = _decodedWidth = _decodedHeight = 0;
    _sourceSize = _sourceWriteTime = 0;
  }

  uint16_t *decode(const String &path, FileFormats::Format format) {
    reset();
    _failure = "";
    _owner = this;
    if (!sourceInfo(path)) {
      _failure = F("source open/stat failed");
      thumbnailLog(String(F("source unavailable ")) + path);
      _owner = nullptr;
      return nullptr;
    }
    bool ok = loadCache(path, format);
    if (ok) thumbnailLog(String(F("cache hit ")) + path);
    if (!ok) {
      thumbnailLog(String(F("cache miss; decode ")) + path);
      ok = format == FileFormats::Format::Bmp ? decodeBmp(path) :
           format == FileFormats::Format::Jpeg ? decodeJpeg(path) :
           format == FileFormats::Format::Png ? decodePng(path) :
           format == FileFormats::Format::Gif ? decodeGif(path) :
           format == FileFormats::Format::Webp ? decodeWebp(path) : false;
      if (ok) writeCache(path, format);
    }
    _owner = nullptr;
    if (!ok) {
      if (_failure.length() == 0) _failure = F("decoder rejected source");
      reset();
      return nullptr;
    }
    uint16_t *pixels = _pixels;
    _pixels = nullptr;
    closeFile();
    return pixels;
  }

  bool prepare(uint16_t sourceWidth, uint16_t sourceHeight, uint16_t decodedWidth, uint16_t decodedHeight) {
    if (!sourceWidth || !sourceHeight || !decodedWidth || !decodedHeight) {
      _failure = F("invalid image dimensions");
      return false;
    }
    _pixels = static_cast<uint16_t *>(psramFound() ? ps_malloc(Size * Size * 2) : malloc(Size * Size * 2));
    if (!_pixels) {
      _failure = F("thumbnail buffer allocation failed");
      return false;
    }
    for (uint16_t i = 0; i < Size * Size; i++) _pixels[i] = TFT_BLACK;
    _sourceWidth = sourceWidth;
    _sourceHeight = sourceHeight;
    _decodedWidth = decodedWidth;
    _decodedHeight = decodedHeight;
    return true;
  }

  void store(uint16_t x, uint16_t y, uint16_t color, bool decoded) {
    if (!_pixels) return;
    uint16_t width = decoded ? _decodedWidth : _sourceWidth;
    uint16_t height = decoded ? _decodedHeight : _sourceHeight;
    const uint16_t left = (uint32_t)x * Size / width;
    uint16_t right = ((uint32_t)(x + 1) * Size + width - 1) / width;
    const uint16_t top = (uint32_t)y * Size / height;
    uint16_t bottom = ((uint32_t)(y + 1) * Size + height - 1) / height;
    if (right > Size) right = Size;
    if (bottom > Size) bottom = Size;
    for (uint16_t targetY = top; targetY < bottom; targetY++) {
      for (uint16_t targetX = left; targetX < right; targetX++) _pixels[targetY * Size + targetX] = color;
    }
  }

  bool openFile(const char *path, int32_t *size = nullptr) {
    closeFile();
    if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
    _file = SD.open(path, FILE_READ);
    if (size && _file) *size = _file.size();
    SpiBusLock::unlock();
    return (bool)_file;
  }
  void closeFile() {
    if (!_file) return;
    if (SpiBusLock::lock(pdMS_TO_TICKS(20))) { _file.close(); SpiBusLock::unlock(); }
  }
  int32_t readFile(uint8_t *data, int32_t length) {
    if (!_file || !SpiBusLock::lock(pdMS_TO_TICKS(20))) return 0;
    int32_t got = _file.read(data, length);
    SpiBusLock::unlock();
    return got;
  }
  bool seekFile(uint32_t offset) {
    if (!_file || !SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
    bool ok = _file.seek(offset);
    SpiBusLock::unlock();
    return ok;
  }

  bool sourceInfo(const String &path) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
    File source = SD.open(path.c_str(), FILE_READ);
    const bool opened = (bool)source;
    if (source) {
      _sourceSize = source.size();
      _sourceWriteTime = (uint32_t)source.getLastWrite();
      source.close();
    }
    SpiBusLock::unlock();
    return opened;
  }

  String cacheDirectoryFor(const String &path) const {
    const int slash = path.lastIndexOf('/');
    const String parent = slash <= 0 ? "/" : path.substring(0, slash);
    return parent == "/" ? "/.cache" : parent + "/.cache";
  }

  String cachePathFor(const String &path) const {
    return cacheDirectoryFor(path) + "/" + path.substring(path.lastIndexOf('/') + 1) + ".eth1";
  }

  static uint32_t readLe32(const uint8_t *value) {
    return (uint32_t)value[0] | ((uint32_t)value[1] << 8) |
           ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24);
  }

  static void writeLe32(uint8_t *value, uint32_t number) {
    value[0] = (uint8_t)number;
    value[1] = (uint8_t)(number >> 8);
    value[2] = (uint8_t)(number >> 16);
    value[3] = (uint8_t)(number >> 24);
  }

  bool readCacheFile(File &file, uint8_t *data, size_t length) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
    const size_t read = file.read(data, length);
    SpiBusLock::unlock();
    return read == length;
  }

  bool writeCacheFile(File &file, const uint8_t *data, size_t length) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
    const size_t written = file.write(data, length);
    SpiBusLock::unlock();
    return written == length;
  }

  void closeCacheFile(File &file) {
    if (!file) return;
    if (SpiBusLock::lock(pdMS_TO_TICKS(20))) {
      file.close();
      SpiBusLock::unlock();
    }
  }

  bool loadCache(const String &sourcePath, FileFormats::Format format) {
    const String path = cachePathFor(sourcePath);
    if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
    File cache = SD.open(path.c_str(), FILE_READ);
    const size_t size = cache ? cache.size() : 0;
    SpiBusLock::unlock();
    if (!cache || size != CacheHeaderBytes + Size * Size * sizeof(uint16_t)) {
      closeCacheFile(cache);
      return false;
    }
    uint8_t header[CacheHeaderBytes] = {};
    const bool valid = readCacheFile(cache, header, sizeof(header)) &&
      memcmp(header, "ETH1", 4) == 0 && header[4] == 1 && header[5] == (uint8_t)format &&
      header[6] == Size && header[7] == Size && readLe32(header + 8) == _sourceSize &&
      readLe32(header + 12) == _sourceWriteTime && readLe32(header + 16) == Size * Size * sizeof(uint16_t);
    if (!valid || !prepare(Size, Size, Size, Size) ||
        !readCacheFile(cache, reinterpret_cast<uint8_t *>(_pixels), Size * Size * sizeof(uint16_t))) {
      closeCacheFile(cache);
      if (_pixels) { free(_pixels); _pixels = nullptr; }
      return false;
    }
    closeCacheFile(cache);
    return true;
  }

  void writeCache(const String &sourcePath, FileFormats::Format format) {
    if (!_pixels || _sourceSize == 0) return;
    const String finalPath = cachePathFor(sourcePath);
    const String temporaryPath = finalPath + ".tmp";
    if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return;
    SD.mkdir(cacheDirectoryFor(sourcePath).c_str());
    SD.remove(temporaryPath.c_str());
    File cache = SD.open(temporaryPath.c_str(), FILE_WRITE);
    SpiBusLock::unlock();
    if (!cache) return;
    uint8_t header[CacheHeaderBytes] = {'E', 'T', 'H', '1', 1, (uint8_t)format, Size, Size};
    writeLe32(header + 8, _sourceSize);
    writeLe32(header + 12, _sourceWriteTime);
    writeLe32(header + 16, Size * Size * sizeof(uint16_t));
    bool written = writeCacheFile(cache, header, sizeof(header));
    const uint8_t *pixels = reinterpret_cast<const uint8_t *>(_pixels);
    for (size_t offset = 0; written && offset < Size * Size * sizeof(uint16_t); offset += 256) {
      const size_t amount = Size * Size * sizeof(uint16_t) - offset > 256 ? 256 : Size * Size * sizeof(uint16_t) - offset;
      written = writeCacheFile(cache, pixels + offset, amount);
    }
    closeCacheFile(cache);
    if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return;
    if (written) {
      SD.remove(finalPath.c_str());
      if (!SD.rename(temporaryPath.c_str(), finalPath.c_str())) SD.remove(temporaryPath.c_str());
    } else {
      SD.remove(temporaryPath.c_str());
    }
    SpiBusLock::unlock();
  }

  static void *openCallback(const char *path, int32_t *size) { return _owner && _owner->openFile(path, size) ? &_owner->_file : nullptr; }
  static void closeCallback(void *handle) { (void)handle; if (_owner) _owner->closeFile(); }
  static int32_t jpegRead(JPEGFILE *file, uint8_t *data, int32_t length) { (void)file; return _owner ? _owner->readFile(data, length) : 0; }
  static int32_t jpegSeek(JPEGFILE *file, int32_t position) { (void)file; return _owner && position >= 0 && _owner->seekFile(position) ? position : 0; }
  static int jpegDraw(JPEGDRAW *draw) {
    if (!_owner || !draw || !draw->pPixels) return 0;
    for (int y = 0; y < draw->iHeight; y++) for (int x = 0; x < draw->iWidthUsed; x++)
      _owner->store(draw->x + x, draw->y + y, draw->pPixels[y * draw->iWidth + x], true);
    return 1;
  }
  static int32_t pngRead(PNGFILE *file, uint8_t *data, int32_t length) { (void)file; return _owner ? _owner->readFile(data, length) : 0; }
  static int32_t pngSeek(PNGFILE *file, int32_t position) {
    if (!file || !_owner || position < 0 || !_owner->seekFile(position)) return 0;
    file->iPos = position;
    return position;
  }
  static int pngDraw(PNGDRAW *draw) {
    if (!_owner || !draw) return 0;
    uint16_t *line = static_cast<uint16_t *>(malloc((size_t)draw->iWidth * 2));
    if (!line) return 0;
    _owner->_png.getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, 0);
    for (int x = 0; x < draw->iWidth; x++) _owner->store(x, draw->y, line[x], false);
    free(line);
    return 1;
  }
  static int32_t gifRead(GIFFILE *file, uint8_t *data, int32_t length) { (void)file; return _owner ? _owner->readFile(data, length) : 0; }
  static int32_t gifSeek(GIFFILE *file, int32_t position) {
    if (!file || !_owner || position < 0 || !_owner->seekFile(position)) return 0;
    file->iPos = position;
    return position;
  }
  static void gifDraw(GIFDRAW *draw) {
    if (!_owner || !draw || !draw->pPixels || !draw->pPalette) return;
    for (int x = 0; x < draw->iWidth; x++) {
      uint8_t index = draw->pPixels[x];
      if (!draw->ucHasTransparency || index != draw->ucTransparent)
        _owner->store(draw->iX + x, draw->iY + draw->y, draw->pPalette[index], false);
    }
  }

  bool decodeJpeg(const String &path) {
    if (_jpeg.open(path.c_str(), openCallback, closeCallback, jpegRead, jpegSeek, jpegDraw) != 1) {
      _failure = String(F("JPEG open error ")) + String(_jpeg.getLastError());
      return false;
    }
    if (_jpeg.getJPEGType() != JPEG_MODE_BASELINE) {
      _failure = F("JPEG is not baseline");
      _jpeg.close();
      return false;
    }
    uint16_t width = _jpeg.getWidth(), height = _jpeg.getHeight();
    uint8_t scale = 1;
    while (scale < 8 && (width / scale > Size * 2 || height / scale > Size * 2)) scale *= 2;
    if (!prepare(width, height, (width + scale - 1) / scale, (height + scale - 1) / scale)) return false;
    int options = scale == 8 ? JPEG_SCALE_EIGHTH : scale == 4 ? JPEG_SCALE_QUARTER : scale == 2 ? JPEG_SCALE_HALF : 0;
    bool ok = _jpeg.decode(0, 0, options) == 1;
    if (!ok) _failure = String(F("JPEG decode error ")) + String(_jpeg.getLastError());
    _jpeg.close();
    return ok;
  }
  bool decodePng(const String &path) {
    int openResult = _png.open(path.c_str(), openCallback, closeCallback, pngRead, pngSeek, pngDraw);
    if (openResult != PNG_SUCCESS) {
      _failure = String(F("PNG open error ")) + String(openResult);
      _png.close();
      return false;
    }
    if (_png.isInterlaced()) { _failure = F("PNG interlaced"); _png.close(); return false; }
    if (!prepare(_png.getWidth(), _png.getHeight(), _png.getWidth(), _png.getHeight())) { _png.close(); return false; }
    bool ok = _png.decode(this, PNG_CHECK_CRC) == PNG_SUCCESS;
    if (!ok) _failure = String(F("PNG decode error ")) + String(_png.getLastError());
    _png.close();
    return ok;
  }
  bool decodeGif(const String &path) {
    _gif = new AnimatedGIF();
    if (!_gif) { _failure = F("GIF decoder allocation failed"); return false; }
    _gif->begin(GIF_PALETTE_RGB565_LE);
    if (!_gif->open(path.c_str(), openCallback, closeCallback, gifRead, gifSeek, gifDraw)) {
      _failure = String(F("GIF open error ")) + String(_gif->getLastError());
      return false;
    }
    if (!prepare(_gif->getCanvasWidth(), _gif->getCanvasHeight(), _gif->getCanvasWidth(), _gif->getCanvasHeight())) return false;
    int delay = 0;
    const bool ok = _gif->playFrame(false, &delay, nullptr) >= 0 && _gif->getLastError() == GIF_SUCCESS;
    if (!ok) _failure = String(F("GIF frame error ")) + String(_gif->getLastError());
    return ok;
  }
  static uint16_t le16(const uint8_t *value) { return (uint16_t)value[0] | ((uint16_t)value[1] << 8); }
  static uint32_t le32(const uint8_t *value) { return (uint32_t)value[0] | ((uint32_t)value[1] << 8) | ((uint32_t)value[2] << 16) | ((uint32_t)value[3] << 24); }
  bool decodeBmp(const String &path) {
    if (!openFile(path.c_str())) { _failure = F("BMP source open failed"); return false; }
    uint8_t header[54] = {};
    if (readFile(header, 54) != 54) { _failure = F("BMP header read failed"); return false; }
    if (header[0] != 'B' || header[1] != 'M') { _failure = F("BMP signature invalid"); return false; }
    uint32_t offset = le32(header + 10), compression = le32(header + 30);
    int32_t width = (int32_t)le32(header + 18), height = (int32_t)le32(header + 22);
    uint16_t bpp = le16(header + 28);
    uint32_t sourceHeight = height < 0 ? (uint32_t)-height : height;
    const bool palette = bpp == 1 || bpp == 4 || bpp == 8;
    const bool direct = bpp == 16 || bpp == 24 || bpp == 32;
    thumbnailLog(String(F("BMP ")) + String(width) + F("x") + String(sourceHeight) + F(" bpp=") + String(bpp) +
                 F(" compression=") + String(compression) + F(" bytes=") + String(_sourceSize));
    if (width <= 0 || height == 0 || width > 65535 || sourceHeight > 65535 || (!palette && !direct) ||
        (palette && compression != 0) || (bpp != 16 && bpp != 24 && bpp != 32 && !palette) ||
        (bpp == 16 && compression != 0 && compression != 3) ||
        (bpp == 24 && compression != 0) || (bpp == 32 && compression != 0 && compression != 3) ||
        !prepare(width, sourceHeight, width, sourceHeight)) {
      if (_failure.length() == 0) _failure = String(F("BMP unsupported: bpp=")) + String(bpp) + F(" compression=") + String(compression);
      return false;
    }
    uint32_t rowBytes = (((uint32_t)width * bpp + 31) / 32) * 4;
    const uint32_t dibSize = le32(header + 14);
    const uint32_t paletteOffset = 14 + dibSize;
    for (uint16_t y = 0; y < Size; y++) for (uint16_t x = 0; x < Size; x++) {
      uint32_t sy = (uint32_t)y * sourceHeight / Size;
      if (height > 0) sy = sourceHeight - 1 - sy;
      uint32_t sx = (uint32_t)x * width / Size;
      uint8_t pixel[4] = {};
      uint16_t color = 0;
      if (palette) {
        const uint32_t bitOffset = sx * bpp;
        if (!seekFile(offset + sy * rowBytes + bitOffset / 8) || readFile(pixel, 1) != 1) {
          _failure = F("BMP palette pixel read failed");
          return false;
        }
        const uint8_t shift = 8 - bpp - (bitOffset & 7);
        const uint8_t paletteIndex = (pixel[0] >> shift) & ((1 << bpp) - 1);
        if (!seekFile(paletteOffset + paletteIndex * 4) || readFile(pixel, 4) != 4) {
          _failure = F("BMP palette entry read failed");
          return false;
        }
        color = ((uint16_t)(pixel[2] & 0xf8) << 8) | ((uint16_t)(pixel[1] & 0xfc) << 3) | (pixel[0] >> 3);
      } else if (bpp == 16) {
        if (!seekFile(offset + sy * rowBytes + sx * 2) || readFile(pixel, 2) != 2) {
          _failure = F("BMP 16-bit pixel read failed");
          return false;
        }
        const uint16_t value = le16(pixel);
        color = compression == 3 ? value :
          (uint16_t)(((value & 0x7c00) << 1) | ((value & 0x03e0) << 1) | (value & 0x001f));
      } else {
        const uint8_t bytes = bpp / 8;
        if (!seekFile(offset + sy * rowBytes + sx * bytes) || readFile(pixel, bytes) != bytes) {
          _failure = F("BMP direct pixel read failed");
          return false;
        }
        color = ((uint16_t)(pixel[2] & 0xf8) << 8) | ((uint16_t)(pixel[1] & 0xfc) << 3) | (pixel[0] >> 3);
      }
      _pixels[y * Size + x] = color;
    }
    return true;
  }
  bool decodeWebp(const String &path) {
    if (!openFile(path.c_str())) { _failure = F("WebP source open failed"); return false; }
    int32_t size = 0;
    if (SpiBusLock::lock(pdMS_TO_TICKS(20))) { size = _file.size(); SpiBusLock::unlock(); }
    if (size <= 0) { _failure = F("WebP source is empty"); return false; }
    if (size > 8 * 1024 * 1024) {
      _failure = String(F("WebP source exceeds 8 MiB: ")) + String(size);
      return false;
    }
    uint8_t *source = static_cast<uint8_t *>(psramFound() ? ps_malloc(size) : malloc(size));
    if (!source) { _failure = String(F("WebP source allocation failed: ")) + String(size); return false; }
    for (int32_t offset = 0; offset < size;) {
      int32_t amount = size - offset > 4096 ? 4096 : size - offset;
      if (readFile(source + offset, amount) != amount) {
        _failure = String(F("WebP source read failed at ")) + String(offset);
        free(source);
        return false;
      }
      offset += amount;
    }
    WebPBitstreamFeatures features;
    const VP8StatusCode featureStatus = WebPGetFeatures(source, size, &features);
    if (featureStatus != VP8_STATUS_OK || features.width <= 0 || features.height <= 0) {
      _failure = String(F("WebP header status ")) + String((int)featureStatus);
      free(source);
      return false;
    }
    if ((uint32_t)features.width * features.height > 4000000UL) {
      _failure = String(F("WebP canvas too large: ")) + String(features.width) + F("x") + String(features.height);
      free(source);
      return false;
    }
    if (features.has_animation && size > 4 * 1024 * 1024) {
      _failure = String(F("Animated WebP source exceeds 4 MiB: ")) + String(size);
      free(source);
      return false;
    }
    if (!prepare(features.width, features.height, features.width, features.height)) {
      free(source);
      return false;
    }
    thumbnailLog(String(F("WebP ")) + String(features.width) + F("x") + String(features.height) +
                 (features.has_animation ? F(" animated") : F(" static")));
    uint8_t *rgba = nullptr;
    int decodedWidth = features.width;
    int decodedHeight = features.height;
    if (features.has_animation) {
      WebPData data = {source, (size_t)size};
      WebPAnimDecoderOptions options;
      WebPAnimInfo info;
      if (!WebPAnimDecoderOptionsInit(&options)) {
        _failure = F("WebP animation options failed");
        free(source);
        return false;
      }
      options.color_mode = MODE_RGBA;
      options.use_threads = 0;
      WebPAnimDecoder *decoder = WebPAnimDecoderNew(&data, &options);
      int timestamp = 0;
      if (!decoder || !WebPAnimDecoderGetInfo(decoder, &info) ||
          !WebPAnimDecoderGetNext(decoder, &rgba, &timestamp) || !rgba) {
        _failure = F("WebP first animation frame decode failed");
        if (decoder) WebPAnimDecoderDelete(decoder);
        free(source);
        return false;
      }
      decodedWidth = info.canvas_width;
      decodedHeight = info.canvas_height;
      if (decodedWidth <= 0 || decodedHeight <= 0) {
        _failure = F("WebP animation canvas invalid");
        WebPAnimDecoderDelete(decoder);
        free(source);
        return false;
      }
      // The animation decoder owns rgba; convert the thumbnail before deleting it.
      for (uint16_t y = 0; y < Size; y++) for (uint16_t x = 0; x < Size; x++) {
        const uint32_t sx = (uint32_t)x * decodedWidth / Size, sy = (uint32_t)y * decodedHeight / Size;
        const uint8_t *pixel = rgba + (sy * decodedWidth + sx) * 4;
        uint16_t red = (uint16_t)pixel[0] * pixel[3] / 255, green = (uint16_t)pixel[1] * pixel[3] / 255, blue = (uint16_t)pixel[2] * pixel[3] / 255;
        _pixels[y * Size + x] = (red & 0xf8) << 8 | (green & 0xfc) << 3 | (blue >> 3);
      }
      WebPAnimDecoderDelete(decoder);
      free(source);
      return true;
    }
    uint8_t scaled[Size * Size * 4] = {};
    WebPDecoderConfig config;
    if (!WebPInitDecoderConfig(&config)) {
      _failure = F("WebP decoder config failed");
      free(source);
      return false;
    }
    config.options.use_scaling = 1;
    config.options.scaled_width = Size;
    config.options.scaled_height = Size;
    config.output.colorspace = MODE_RGBA;
    config.output.is_external_memory = 1;
    config.output.u.RGBA.rgba = scaled;
    config.output.u.RGBA.size = sizeof(scaled);
    config.output.u.RGBA.stride = Size * 4;
    const VP8StatusCode status = WebPDecode(source, size, &config);
    free(source);
    if (status != VP8_STATUS_OK) {
      _failure = String(F("WebP scaled decode status ")) + String((int)status);
      return false;
    }
    for (uint16_t y = 0; y < Size; y++) for (uint16_t x = 0; x < Size; x++) {
      const uint8_t *pixel = scaled + (y * Size + x) * 4;
      uint16_t red = (uint16_t)pixel[0] * pixel[3] / 255, green = (uint16_t)pixel[1] * pixel[3] / 255, blue = (uint16_t)pixel[2] * pixel[3] / 255;
      _pixels[y * Size + x] = (red & 0xf8) << 8 | (green & 0xfc) << 3 | (blue >> 3);
    }
    return true;
  }
};

FileThumbnailWorker *FileThumbnailWorker::_owner = nullptr;

FilesScreen::FilesScreen(Navigator &navigator,
                         TouchController &touch,
                         DisplayController &display,
                         SdCardController &sdCard)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _currentPath("/"),
    _pendingRenamePath(),
    _message(),
    _messageUntilMs(0),
    _page(0),
    _sdMounted(false),
    _lastTouchDown(false),
    _retainCacheWhileCovered(false),
    _listing(nullptr),
    _thumbnailWorker(new FileThumbnailWorker()),
    _listingGeneration(1),
    _rows(nullptr),
    _rowCapacity(0),
    _rowCount(0),
    _rowSlots(1),
    _pageCount(1),
    _previousBounds{0, 0, 0, 0},
    _nextBounds{0, 0, 0, 0},
    _menuBounds{0, 0, 0, 0},
    _renameBounds{0, 0, 0, 0},
    _deleteBounds{0, 0, 0, 0},
    _menuState(MenuState::None),
    _menuEntryIndex(InvalidEntryIndex),
    _deleteStartedMs(0),
    _lastDeleteDrawMs(0) {
  for (uint8_t i = 0; i < MaxCachedDirectories; i++) {
    _cache[i] = {"", nullptr, 0, 0, false};
  }
  for (uint8_t i = 0; i < MaxCachedThumbnails; i++) {
    _thumbnails[i] = {"", nullptr, 0, 0, false, false, false};
  }
}

FilesScreen::~FilesScreen() {
  clearCache();
  clearThumbnails();
  delete[] _rows;
}

void FilesScreen::onEnter() {
  clearCache();
  clearThumbnails();
  _listingGeneration++;
  _currentPath = "/";
  _page = 0;
  _lastTouchDown = false;
  _retainCacheWhileCovered = false;
  _sdMounted = _sdCard.isMounted();
  if (_sdMounted && !ensureListing(_currentPath)) showMessage(F("Unable to list folder"));
  draw();
}

void FilesScreen::onExit() {
  if (!_retainCacheWhileCovered) {
    clearCache();
    clearThumbnails();
    _pendingRenamePath = "";
  }
}

void FilesScreen::onResume() {
  _lastTouchDown = false;
  _retainCacheWhileCovered = false;
  _sdMounted = _sdCard.isMounted();
  draw();
}

void FilesScreen::onResult(const ScreenResult &result) {
  if (result.kind == ScreenResultKind::Text && _pendingRenamePath.length()) {
    applyRename(result.text);
  }
  _pendingRenamePath = "";
}

void FilesScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    if (_menuState == MenuState::DeleteHolding) {
      _menuState = MenuState::Actions;
      drawContextMenu();
    }
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

  if (UiKit::navigationIconContains(point.x, point.y)) {
    _navigator.pop();
    return;
  }
  if (!_sdMounted) return;

  if (_menuState != MenuState::None) {
    if (_renameBounds.contains(point.x, point.y)) {
      startRename();
    } else if (_deleteBounds.contains(point.x, point.y)) {
      beginDeleteHold();
    } else if (!_menuBounds.contains(point.x, point.y)) {
      closeMenu();
    }
    return;
  }

  if (_previousBounds.contains(point.x, point.y)) {
    if (_page > 0) {
      _page--;
      redrawListingArea();
    }
    return;
  }
  if (_nextBounds.contains(point.x, point.y)) {
    if (_page + 1 < _pageCount) {
      _page++;
      redrawListingArea();
    }
    return;
  }

  for (uint8_t i = 0; i < _rowCount; i++) {
    const Row &row = _rows[i];
    if (!row.bounds.contains(point.x, point.y)) continue;
    if (row.parent) {
      openParentDirectory();
    } else if (row.entryIndex != InvalidEntryIndex) {
      Entry *entry = entryAt(row.entryIndex);
      if (entry && !entry->directory && point.x >= _display.width() - ActionWidth) showMenu(row.entryIndex);
      else openEntry(row.entryIndex);
    }
    return;
  }
}

void FilesScreen::tick(uint32_t nowMs) {
  handleSdCardEvent(_sdCard.takeEvent());
  collectThumbnailResults();
  if (_menuState == MenuState::DeleteHolding) updateDeleteHold(nowMs);
  if (_message.length() && (int32_t)(nowMs - _messageUntilMs) >= 0) {
    _message = "";
    const Rect bounds = statusBounds();
    _display.tft().fillRect(bounds.x, bounds.y, bounds.w, bounds.h, TFT_BLACK);
    redrawRowsIntersecting(bounds);
  }
}

void FilesScreen::draw() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  drawTopBar();
  if (!_sdMounted) {
    drawUnavailable();
    return;
  }
  drawPathHeader();
  layoutPaging();
  drawListing();
  drawFooter();
  if (_menuState != MenuState::None) drawContextMenu();
  drawStatus();
}

void FilesScreen::drawTopBar() {
  UiKit::drawTopAppBar(_display.tft(), _display.width(), F("Files"));
}

void FilesScreen::drawPathHeader() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(16, 56, _display.width() - 32, 24, TFT_DARKGREY);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString(UiKit::ellipsize(tft, _currentPath, _display.width() - 48, 2), 24, 68, 2);
  tft.setTextDatum(TL_DATUM);
}

void FilesScreen::layoutPaging() {
  const int16_t contentBottom = _display.height() - FooterHeight;
  const int16_t height = contentBottom > RowTop ? contentBottom - RowTop : UiKit::ListCellHeight;
  _rowSlots = (uint8_t)(height / UiKit::ListCellHeight);
  if (_rowSlots == 0) _rowSlots = 1;
  ensureRowCapacity(_rowSlots);
  const int16_t third = _display.width() / 3;
  const int16_t footerTop = _display.height() - FooterHeight;
  _previousBounds = {0, footerTop, third, FooterHeight};
  _nextBounds = {(int16_t)(third * 2), footerTop, (int16_t)(_display.width() - third * 2), FooterHeight};
  updatePageCount();
}

uint16_t FilesScreen::firstEntryForPage(uint16_t page) const {
  if (page == 0 || !_listing) return 0;
  const uint16_t firstCapacity = entryCapacityForPage(0);
  return firstCapacity + (uint32_t)(page - 1) * entryCapacityForPage(1);
}

uint16_t FilesScreen::entryCapacityForPage(uint16_t page) const {
  if (_currentPath != "/" && page == 0) return _rowSlots > 1 ? _rowSlots - 1 : 0;
  return _rowSlots;
}

void FilesScreen::updatePageCount() {
  const uint16_t count = _listing ? _listing->count : 0;
  const uint16_t firstCapacity = entryCapacityForPage(0);
  const uint16_t laterCapacity = entryCapacityForPage(1);
  if (count <= firstCapacity || laterCapacity == 0) {
    _pageCount = 1;
  } else {
    _pageCount = 1 + (count - firstCapacity + laterCapacity - 1) / laterCapacity;
  }
  if (_page >= _pageCount) _page = _pageCount - 1;
}

void FilesScreen::drawListing() {
  _rowCount = 0;
  auto addRow = [&](int16_t y, uint16_t index, bool parent) -> Row * {
    if (_rowCount >= _rowCapacity) return nullptr;
    Row &row = _rows[_rowCount++];
    row = {{0, y, _display.width(), UiKit::ListCellHeight}, index, parent};
    return &row;
  };

  if (_currentPath != "/" && _page == 0) {
    Row *row = addRow(RowTop, InvalidEntryIndex, true);
    if (row) drawRow(*row);
  }

  const uint16_t first = firstEntryForPage(_page);
  const uint16_t capacity = entryCapacityForPage(_page);
  for (uint16_t offset = 0; offset < capacity; offset++) {
    const uint16_t index = first + offset;
    const Entry *entry = entryAt(index);
    if (!entry) break;
    const int16_t y = RowTop + _rowCount * UiKit::ListCellHeight;
    Row *row = addRow(y, index, false);
    if (!row) break;
    drawRow(*row);
  }
  queueVisibleThumbnails();
}

void FilesScreen::redrawListingArea() {
  const int16_t contentBottom = _display.height() - FooterHeight;
  _display.tft().fillRect(0, RowTop, _display.width(), contentBottom - RowTop, TFT_BLACK);
  drawListing();
  drawFooter();
  drawStatus();
}

void FilesScreen::drawRow(const Row &row) {
  if (row.parent) {
    UiKit::drawListCell(_display.tft(), _display.width(), row.bounds.y, F(".."), ic_dir, FolderColor);
    return;
  }
  const Entry *entry = entryAt(row.entryIndex);
  if (!entry) return;
  const uint8_t *icon = entry->directory ? ic_dir : ic_file;
  uint16_t color = FolderColor;
  if (!entry->directory) {
    const bool openable = FileFormats::isOpenable(entry->format);
    color = openable && FileFormats::isImage(entry->format) ? ImageFileColor
          : openable && FileFormats::isAudio(entry->format) ? AudioFileColor
          : openable && FileFormats::isVideo(entry->format) ? VideoFileColor : FileColor;
  }
  UiKit::drawListCell(_display.tft(), _display.width(), row.bounds.y, entry->name, icon, color,
                      entry->directory ? String() : String(F("            ")));
  if (!entry->directory) {
    const String path = fullPathFor(*entry);
    Thumbnail *thumbnail = thumbnailFor(path);
    if (thumbnail && thumbnail->pixels) _display.tft().pushImage(8, row.bounds.y + 4, FileThumbnailWorker::Size,
                                                                  FileThumbnailWorker::Size, thumbnail->pixels);
    drawVerticalEllipsis(_display.width() - ActionWidth / 2, row.bounds.y + UiKit::ListCellHeight / 2, TFT_LIGHTGREY);
  }
}

void FilesScreen::redrawRowsIntersecting(const Rect &bounds) {
  for (uint8_t i = 0; i < _rowCount; i++) {
    const Rect &row = _rows[i].bounds;
    if (row.y + row.h <= bounds.y || bounds.y + bounds.h <= row.y) continue;
    drawRow(_rows[i]);
  }
}

void FilesScreen::drawFooter() {
  TFT_eSPI &tft = _display.tft();
  const int16_t footerTop = _display.height() - FooterHeight;
  const bool previous = _page > 0;
  const bool next = _page + 1 < _pageCount;
  tft.fillRect(0, footerTop, _display.width(), FooterHeight, MenuColor);
  tft.drawFastVLine(_previousBounds.w, footerTop, FooterHeight, TFT_DARKGREY);
  tft.drawFastVLine(_nextBounds.x, footerTop, FooterHeight, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  drawPagingArrow(_previousBounds.x + _previousBounds.w / 2, footerTop + FooterHeight / 2,
                  false, previous ? TFT_WHITE : 0x8410);
  tft.setTextColor(TFT_LIGHTGREY, MenuColor);
  tft.drawString(String(F("Page ")) + String(_page + 1) + F(" / ") + String(_pageCount),
                 _display.width() / 2, footerTop + FooterHeight / 2, 2);
  drawPagingArrow(_nextBounds.x + _nextBounds.w / 2, footerTop + FooterHeight / 2,
                  true, next ? TFT_WHITE : 0x8410);
  tft.setTextDatum(TL_DATUM);
}

void FilesScreen::drawContextMenu() {
  if (_menuEntryIndex == InvalidEntryIndex || !entryAt(_menuEntryIndex)) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t menuWidth = 150;
  const int16_t menuHeight = 76;
  const int16_t x = _display.width() - menuWidth - 8;
  const int16_t y = RowTop + 4;
  _menuBounds = {x, y, menuWidth, menuHeight};
  _renameBounds = {x, y, menuWidth, menuHeight / 2};
  _deleteBounds = {x, (int16_t)(y + menuHeight / 2), menuWidth, menuHeight - menuHeight / 2};
  tft.fillRoundRect(x, y, menuWidth, menuHeight, 5, MenuColor);
  tft.drawRoundRect(x, y, menuWidth, menuHeight, 5, TFT_LIGHTGREY);
  tft.drawFastHLine(x + 8, _deleteBounds.y, menuWidth - 16, TFT_DARKGREY);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, MenuColor);
  tft.drawString(F("Rename"), x + 14, _renameBounds.y + _renameBounds.h / 2, 2);
  const bool holding = _menuState == MenuState::DeleteHolding;
  tft.setTextColor(holding ? TFT_WHITE : TFT_RED, holding ? DangerColor : MenuColor);
  if (holding) {
    uint32_t elapsed = millis() - _deleteStartedMs;
    uint8_t percent = elapsed >= DeleteHoldMs ? 100 : (uint8_t)(elapsed * 100UL / DeleteHoldMs);
    tft.fillRect(x + 4, _deleteBounds.y + 3, (menuWidth - 8) * percent / 100, _deleteBounds.h - 6, DangerColor);
  }
  tft.drawString(holding ? F("Keep holding...") : F("Hold to delete"), x + 14,
                 _deleteBounds.y + _deleteBounds.h / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void FilesScreen::drawStatus() {
  if (_message.length() == 0) return;
  TFT_eSPI &tft = _display.tft();
  const Rect bounds = statusBounds();
  tft.fillRoundRect(bounds.x, bounds.y, bounds.w, bounds.h, 5, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString(UiKit::ellipsize(tft, _message, bounds.w - 12, 2), _display.width() / 2, bounds.y + bounds.h / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void FilesScreen::drawUnavailable() {
  TFT_eSPI &tft = _display.tft();
  int16_t centerX = _display.width() / 2;
  tft.fillRect(centerX - 24, 116, 48, 48, TFT_DARKGREY);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(F("SD card unavailable"), centerX, 190, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(F("Insert or mount an SD card"), centerX, 216, 2);
  tft.setTextDatum(TL_DATUM);
}

void FilesScreen::drawVerticalEllipsis(int16_t centerX, int16_t centerY, uint16_t color) const {
  TFT_eSPI &tft = _display.tft();
  tft.fillCircle(centerX, centerY - 6, 2, color);
  tft.fillCircle(centerX, centerY, 2, color);
  tft.fillCircle(centerX, centerY + 6, 2, color);
}

void FilesScreen::drawPagingArrow(int16_t centerX, int16_t centerY, bool forward, uint16_t color) const {
  const int16_t tipX = centerX + (forward ? 7 : -7);
  const int16_t tailX = centerX + (forward ? -8 : 8);
  for (int8_t offset = -1; offset <= 1; offset++) {
    _display.tft().drawLine(tailX, centerY + offset, tipX, centerY + offset, color);
    _display.tft().drawLine(tipX, centerY + offset, centerX + (forward ? 1 : -1), centerY - 7 + offset, color);
    _display.tft().drawLine(tipX, centerY + offset, centerX + (forward ? 1 : -1), centerY + 7 + offset, color);
  }
}

FilesScreen::Rect FilesScreen::statusBounds() const {
  return {16, (int16_t)(_display.height() - FooterHeight - 30), (int16_t)(_display.width() - 32), 24};
}

FilesScreen::Thumbnail *FilesScreen::thumbnailFor(const String &path) {
  for (uint8_t i = 0; i < MaxCachedThumbnails; i++) {
    Thumbnail &thumbnail = _thumbnails[i];
    if (thumbnail.used && thumbnail.path == path) {
      thumbnail.lastUsed = millis();
      return &thumbnail;
    }
  }
  return nullptr;
}

FilesScreen::Thumbnail *FilesScreen::reserveThumbnail(const String &path) {
  Thumbnail *oldest = nullptr;
  for (uint8_t i = 0; i < MaxCachedThumbnails; i++) {
    Thumbnail &thumbnail = _thumbnails[i];
    if (!thumbnail.used) {
      thumbnail = {path, nullptr, millis(), _listingGeneration, true, false, false};
      return &thumbnail;
    }
    if (!thumbnail.pending && (!oldest || thumbnail.lastUsed < oldest->lastUsed)) oldest = &thumbnail;
  }
  if (!oldest) return nullptr;
  if (oldest->pixels) free(oldest->pixels);
  *oldest = {path, nullptr, millis(), _listingGeneration, true, false, false};
  return oldest;
}

void FilesScreen::queueVisibleThumbnails() {
  if (!_thumbnailWorker || !_listing) return;
  for (uint8_t i = 0; i < _rowCount; i++) {
    const Row &row = _rows[i];
    if (row.parent || row.entryIndex == InvalidEntryIndex) continue;
    Entry *entry = entryAt(row.entryIndex);
    if (!entry || entry->directory || !FileFormats::isImage(entry->format) || !FileFormats::isOpenable(entry->format)) continue;
    const String path = fullPathFor(*entry);
    Thumbnail *thumbnail = thumbnailFor(path);
    if (!thumbnail) thumbnail = reserveThumbnail(path);
    if (!thumbnail || thumbnail->pixels || thumbnail->pending || thumbnail->failed) continue;
    if (_thumbnailWorker->enqueue(path, entry->format, _listingGeneration)) {
      thumbnail->pending = true;
      thumbnailLog(String(F("queued ")) + path);
    } else if (!thumbnail->pixels) {
      thumbnail->used = false;
      thumbnail->path = "";
      thumbnailLog(String(F("queue full or path too long ")) + path);
    }
  }
}

void FilesScreen::collectThumbnailResults() {
  if (!_thumbnailWorker) return;
  FileThumbnailWorker::Result result{};
  while (_thumbnailWorker->take(result)) {
    if (result.generation != _listingGeneration) {
      if (result.pixels) free(result.pixels);
      continue;
    }
    Thumbnail *thumbnail = thumbnailFor(String(result.path));
    if (!thumbnail || !thumbnail->pending) {
      if (result.pixels) free(result.pixels);
      continue;
    }
    thumbnail->pending = false;
    thumbnail->pixels = result.pixels;
    thumbnail->lastUsed = millis();
    thumbnail->failed = result.pixels == nullptr;
    if (thumbnail->failed) thumbnailLog(String(F("leaving file icon ")) + thumbnail->path);
    if (result.pixels) {
      for (uint8_t i = 0; i < _rowCount; i++) {
        Entry *entry = !_rows[i].parent && _rows[i].entryIndex != InvalidEntryIndex
          ? entryAt(_rows[i].entryIndex) : nullptr;
        if (entry && fullPathFor(*entry) == thumbnail->path) {
          const Rect &bounds = _rows[i].bounds;
          const bool overlapsMenu = _menuState != MenuState::None && _menuBounds.w > 0 &&
            bounds.y < _menuBounds.y + _menuBounds.h && _menuBounds.y < bounds.y + bounds.h;
          drawRow(_rows[i]);
          if (overlapsMenu) drawContextMenu();
          break;
        }
      }
    }
    // A worker result frees one of the two job slots. Fill it immediately so
    // every visible image is eventually decoded without requiring a redraw or
    // directory revisit.
    queueVisibleThumbnails();
  }
}

void FilesScreen::clearThumbnails() {
  for (uint8_t i = 0; i < MaxCachedThumbnails; i++) {
    if (_thumbnails[i].pixels) free(_thumbnails[i].pixels);
    _thumbnails[i] = {"", nullptr, 0, 0, false, false, false};
  }
}

void FilesScreen::invalidateThumbnail(const String &path) {
  Thumbnail *thumbnail = thumbnailFor(path);
  if (!thumbnail) return;
  if (thumbnail->pixels) free(thumbnail->pixels);
  *thumbnail = {"", nullptr, 0, 0, false, false, false};
}

void FilesScreen::ensureRowCapacity(uint8_t capacity) {
  if (capacity == 0 || capacity == _rowCapacity) return;
  Row *rows = new Row[capacity];
  if (!rows) return;
  delete[] _rows;
  _rows = rows;
  _rowCapacity = capacity;
}

bool FilesScreen::ensureListing(const String &path) {
  DirectoryCache *cached = findCached(path);
  if (!cached) {
    cached = allocateCacheSlot();
    if (!cached || !loadDirectory(*cached, path)) return false;
  }
  cached->lastUsed = millis();
  _listing = cached;
  sortListing();
  updatePageCount();
  return true;
}

FilesScreen::DirectoryCache *FilesScreen::findCached(const String &path) {
  for (uint8_t i = 0; i < MaxCachedDirectories; i++) {
    if (_cache[i].used && _cache[i].path == path) return &_cache[i];
  }
  return nullptr;
}

FilesScreen::DirectoryCache *FilesScreen::allocateCacheSlot() {
  DirectoryCache *oldest = nullptr;
  for (uint8_t i = 0; i < MaxCachedDirectories; i++) {
    if (!_cache[i].used) return &_cache[i];
    if (!_listing || &_cache[i] != _listing) {
      if (!oldest || _cache[i].lastUsed < oldest->lastUsed) oldest = &_cache[i];
    }
  }
  if (oldest) releaseCache(*oldest);
  return oldest;
}

void FilesScreen::releaseCache(DirectoryCache &cache) {
  delete[] cache.entries;
  cache = {"", nullptr, 0, 0, false};
  if (&cache == _listing) _listing = nullptr;
}

void FilesScreen::clearCache() {
  for (uint8_t i = 0; i < MaxCachedDirectories; i++) releaseCache(_cache[i]);
  _listing = nullptr;
}

bool FilesScreen::loadDirectory(DirectoryCache &cache, const String &path) {
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  File directory = SD.open(path.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    SpiBusLock::unlock();
    return false;
  }
  uint16_t count = 0;
  File entry = directory.openNextFile();
  while (entry) {
    if (!isHiddenEntry(entry.name()) && count < 0xfffe) count++;
    entry.close();
    entry = directory.openNextFile();
  }
  directory.close();
  SpiBusLock::unlock();

  Entry *entries = count ? new Entry[count] : nullptr;
  if (count && !entries) return false;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    delete[] entries;
    return false;
  }
  directory = SD.open(path.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    SpiBusLock::unlock();
    delete[] entries;
    return false;
  }
  uint16_t index = 0;
  entry = directory.openNextFile();
  while (entry) {
    String name = entry.name();
    if (!isHiddenEntry(name) && index < count) {
      entries[index].name = displayName(name);
      entries[index].directory = entry.isDirectory();
      entries[index].format = entries[index].directory ? FileFormats::Format::Unknown : FileFormats::fromPath(name);
      index++;
    }
    entry.close();
    entry = directory.openNextFile();
  }
  directory.close();
  SpiBusLock::unlock();
  if (index != count) {
    delete[] entries;
    return false;
  }
  cache = {path, entries, count, millis(), true};
  _listingGeneration++;
  return true;
}

void FilesScreen::sortListing() {
  if (_listing && _listing->entries && _listing->count > 1) {
    quickSort(_listing->entries, 0, _listing->count - 1);
  }
}

char FilesScreen::folded(char value) {
  return value >= 'A' && value <= 'Z' ? (char)(value + ('a' - 'A')) : value;
}

int FilesScreen::naturalCompare(const String &left, const String &right) {
  uint16_t l = 0;
  uint16_t r = 0;
  while (l < left.length() && r < right.length()) {
    const char lc = left[l];
    const char rc = right[r];
    if (lc >= '0' && lc <= '9' && rc >= '0' && rc <= '9') {
      uint16_t lStart = l;
      uint16_t rStart = r;
      while (l < left.length() && left[l] == '0') l++;
      while (r < right.length() && right[r] == '0') r++;
      uint16_t lDigits = l;
      uint16_t rDigits = r;
      while (l < left.length() && left[l] >= '0' && left[l] <= '9') l++;
      while (r < right.length() && right[r] >= '0' && right[r] <= '9') r++;
      const uint16_t lLength = l - lDigits;
      const uint16_t rLength = r - rDigits;
      if (lLength != rLength) return lLength < rLength ? -1 : 1;
      for (uint16_t i = 0; i < lLength; i++) {
        if (left[lDigits + i] != right[rDigits + i]) return left[lDigits + i] < right[rDigits + i] ? -1 : 1;
      }
      const uint16_t lZeros = lDigits - lStart;
      const uint16_t rZeros = rDigits - rStart;
      if (lZeros != rZeros) return lZeros < rZeros ? -1 : 1;
      continue;
    }
    const char lFolded = folded(lc);
    const char rFolded = folded(rc);
    if (lFolded != rFolded) return lFolded < rFolded ? -1 : 1;
    l++;
    r++;
  }
  if (l != left.length() || r != right.length()) return l == left.length() ? -1 : 1;
  return left.compareTo(right);
}

int FilesScreen::compareEntries(const Entry &left, const Entry &right) {
  if (left.directory != right.directory) return left.directory ? -1 : 1;
  return naturalCompare(left.name, right.name);
}

void FilesScreen::quickSort(Entry *entries, int32_t left, int32_t right) {
  int32_t i = left;
  int32_t j = right;
  const Entry pivot = entries[left + (right - left) / 2];
  while (i <= j) {
    while (compareEntries(entries[i], pivot) < 0) i++;
    while (compareEntries(entries[j], pivot) > 0) j--;
    if (i <= j) {
      Entry temporary = entries[i];
      entries[i] = entries[j];
      entries[j] = temporary;
      i++;
      j--;
    }
  }
  if (left < j) quickSort(entries, left, j);
  if (i < right) quickSort(entries, i, right);
}

FilesScreen::Entry *FilesScreen::entryAt(uint16_t index) {
  return _listing && index < _listing->count ? &_listing->entries[index] : nullptr;
}

const FilesScreen::Entry *FilesScreen::entryAt(uint16_t index) const {
  return _listing && index < _listing->count ? &_listing->entries[index] : nullptr;
}

bool FilesScreen::isHiddenEntry(const String &path) const {
  const String name = displayName(path);
  return name.startsWith(F("._")) || name == F(".cache");
}

void FilesScreen::openDirectory(const String &path) {
  String normalized = path;
  if (normalized.length() == 0) return;
  if (!normalized.startsWith("/")) normalized = "/" + normalized;
  while (normalized.length() > 1 && normalized.endsWith("/")) normalized.remove(normalized.length() - 1);
  if (!ensureListing(normalized)) {
    showMessage(F("Unable to list folder"));
    draw();
    return;
  }
  _currentPath = normalized;
  _page = 0;
  closeMenu();
  draw();
}

void FilesScreen::openParentDirectory() {
  openDirectory(parentPath());
}

String FilesScreen::parentPath() const {
  if (_currentPath == "/") return "/";
  int16_t slash = _currentPath.lastIndexOf('/');
  return slash <= 0 ? "/" : _currentPath.substring(0, slash);
}

String FilesScreen::fullPathFor(const Entry &entry) const {
  return _currentPath == "/" ? "/" + entry.name : _currentPath + "/" + entry.name;
}

String FilesScreen::displayName(const String &path) const {
  int16_t slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(slash + 1);
}

void FilesScreen::openEntry(uint16_t index) {
  Entry *entry = entryAt(index);
  if (!entry) return;
  if (entry->directory) {
    openDirectory(fullPathFor(*entry));
    return;
  }
  const String path = fullPathFor(*entry);
  _retainCacheWhileCovered = true;
  bool opened = false;
  if (FileFormats::isImage(entry->format) && FileFormats::isOpenable(entry->format)) {
    opened = _navigator.push(ScreenId::ImageViewer, ScreenData::imagePath(path));
  } else if (FileFormats::isAudio(entry->format) && FileFormats::isOpenable(entry->format)) {
    opened = _navigator.push(ScreenId::AudioPlayer, ScreenData::audioPath(path));
  } else if (FileFormats::isVideo(entry->format) && FileFormats::isOpenable(entry->format)) {
    opened = _navigator.push(ScreenId::VideoPlayer, ScreenData::videoPath(path));
  }
  if (!opened) {
    _retainCacheWhileCovered = false;
  }
}

void FilesScreen::showMenu(uint16_t index) {
  Entry *entry = entryAt(index);
  if (!entry || entry->directory) return;
  _menuEntryIndex = index;
  _menuState = MenuState::Actions;
  drawContextMenu();
}

void FilesScreen::closeMenu() {
  const Rect previousBounds = _menuBounds;
  _menuState = MenuState::None;
  _menuEntryIndex = InvalidEntryIndex;
  _deleteStartedMs = 0;
  if (_display.isReady() && previousBounds.w > 0) redrawRowsIntersecting(previousBounds);
}

void FilesScreen::startRename() {
  Entry *entry = entryAt(_menuEntryIndex);
  if (!entry) return;
  _pendingRenamePath = fullPathFor(*entry);
  _retainCacheWhileCovered = true;
  closeMenu();
  if (!_navigator.push(ScreenId::TextInput, ScreenData::textInput("Rename file", false, entry->name))) {
    _retainCacheWhileCovered = false;
    _pendingRenamePath = "";
    showMessage(F("Unable to open keyboard"));
    draw();
  }
}

bool FilesScreen::validFileName(const String &name) const {
  return name.length() > 0 && name != F(".") && name != F("..") && name.indexOf('/') < 0;
}

void FilesScreen::applyRename(const String &newName) {
  if (!validFileName(newName)) {
    showMessage(F("Invalid file name"));
    draw();
    return;
  }
  const int16_t slash = _pendingRenamePath.lastIndexOf('/');
  const String target = (slash <= 0 ? "/" : _pendingRenamePath.substring(0, slash) + "/") + newName;
  if (target == _pendingRenamePath) {
    draw();
    return;
  }
  bool renamed = false;
  if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    if (!SD.exists(target.c_str())) renamed = SD.rename(_pendingRenamePath.c_str(), target.c_str());
    SpiBusLock::unlock();
  }
  if (!renamed) {
    showMessage(F("Rename failed or target exists"));
    draw();
    return;
  }
  for (uint16_t i = 0; _listing && i < _listing->count; i++) {
    if (fullPathFor(_listing->entries[i]) == _pendingRenamePath) {
      _listing->entries[i].name = newName;
      _listing->entries[i].format = FileFormats::fromPath(newName);
      invalidateThumbnail(_pendingRenamePath);
      sortListing();
      break;
    }
  }
  showMessage(F("Renamed"));
  draw();
}

void FilesScreen::beginDeleteHold() {
  if (_menuEntryIndex == InvalidEntryIndex) return;
  _menuState = MenuState::DeleteHolding;
  _deleteStartedMs = millis();
  _lastDeleteDrawMs = 0;
  drawContextMenu();
}

void FilesScreen::updateDeleteHold(uint32_t nowMs) {
  TouchPoint point = _touch.point();
  if (!point.touched || !_deleteBounds.contains(point.x, point.y)) {
    _menuState = MenuState::Actions;
    drawContextMenu();
    return;
  }
  if (nowMs - _deleteStartedMs >= DeleteHoldMs) {
    deleteSelectedFile();
    return;
  }
  if (nowMs - _lastDeleteDrawMs >= 80) {
    _lastDeleteDrawMs = nowMs;
    drawContextMenu();
  }
}

void FilesScreen::deleteSelectedFile() {
  Entry *entry = entryAt(_menuEntryIndex);
  if (!entry || entry->directory) {
    closeMenu();
    return;
  }
  const String path = fullPathFor(*entry);
  bool removed = false;
  if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    removed = SD.remove(path.c_str());
    SpiBusLock::unlock();
  }
  const uint16_t index = _menuEntryIndex;
  closeMenu();
  if (!removed) {
    showMessage(F("Delete failed"));
  } else {
    removeEntryFromListing(index);
    showMessage(F("Deleted"));
  }
  redrawListingArea();
}

void FilesScreen::removeEntryFromListing(uint16_t index) {
  if (!_listing || index >= _listing->count) return;
  invalidateThumbnail(fullPathFor(_listing->entries[index]));
  for (uint16_t i = index + 1; i < _listing->count; i++) _listing->entries[i - 1] = _listing->entries[i];
  _listing->count--;
  updatePageCount();
}

void FilesScreen::showMessage(const String &message, uint32_t durationMs) {
  _message = message;
  _messageUntilMs = millis() + durationMs;
}

void FilesScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::None) return;
  closeMenu();
  if (event == SdCardEvent::Mounted) {
    _sdMounted = true;
    clearCache();
    clearThumbnails();
    _listingGeneration++;
    _currentPath = "/";
    _page = 0;
    if (!ensureListing(_currentPath)) showMessage(F("Unable to list SD card"));
  } else if (event == SdCardEvent::Removed || event == SdCardEvent::MountFailed) {
    _sdMounted = false;
    clearCache();
    clearThumbnails();
    _listingGeneration++;
  }
  draw();
}
