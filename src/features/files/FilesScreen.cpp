#include "FilesScreen.h"

#include "../../app/Navigator.h"
#include "../../core/SpiBusLock.h"
#include "../../ui/icons.h"

#include <FS.h>
#include <SD.h>
#include <JPEGDEC.h>
#ifndef PNG_MAX_BUFFERED_PIXELS
#define PNG_MAX_BUFFERED_PIXELS (((4096 * 4 + 1) * 2) + 32)
#endif
#include <PNGdec.h>
#include <esp32-hal-psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <string.h>

#include "../../../third_party/AnimatedGIF/AnimatedGIF.h"
#include "../../../third_party/libwebp/src/webp/decode.h"
#include "../../../third_party/libwebp/src/webp/demux.h"

using File = fs::File;

namespace {
const uint16_t FolderColor = TFT_YELLOW;
const uint16_t FileColor = TFT_LIGHTGREY;
const uint16_t ImageFileColor = TFT_GREEN;
const uint16_t AudioFileColor = TFT_BLUE;
const uint16_t VideoFileColor = TFT_ORANGE;
const uint16_t MenuColor = 0x2104;
const uint16_t DangerColor = TFT_RED;
const uint16_t FooterButtonColor = 0x2965;
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
  // This matches the 4,096px RGBA scanline buffer configured for PNGdec.
  // A thumbnail never needs a larger source row, and bounding it prevents a
  // malformed PNG header from overrunning PNGdec's fixed two-row buffer.
  static const uint32_t MaxPngDimension = 4096;
  static const uint32_t MaxPngScanlineBytes = MaxPngDimension * 4;
  struct Result { uint32_t generation; char path[MaxPath]; uint16_t *pixels; };

  FileThumbnailWorker(SecretStorage &secretStorage, MediaCache &mediaCache)
    : _secretStorage(secretStorage), _mediaCache(mediaCache), _jobs(xQueueCreate(2, sizeof(Job))), _results(xQueueCreate(2, sizeof(Result))),
      _task(nullptr), _encrypted(false), _pixels(nullptr), _sourceWidth(0), _sourceHeight(0),
      _decodedWidth(0), _decodedHeight(0), _sourceSize(0), _sourceWriteTime(0), _gif(nullptr) {
    if (_jobs && _results) {
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
      const BaseType_t core = 0;
#else
      const BaseType_t core = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
      // Thumbnails are strictly best-effort: they must yield to the Arduino
      // task so a press on the close icon is never delayed by image decoding.
      xTaskCreatePinnedToCore(taskMain, "file-thumbs", 12288, this, tskIDLE_PRIORITY, &_task, core);
    }
  }

  bool enqueue(const String &path, FileFormats::Format format, uint32_t generation, bool encrypted) {
    if (!_task || path.length() == 0 || path.length() >= MaxPath) return false;
    Job job{};
    job.generation = generation;
    job.format = format;
    job.encrypted = encrypted;
    path.toCharArray(job.path, sizeof(job.path));
    return xQueueSend(_jobs, &job, 0) == pdTRUE;
  }

  bool take(Result &result) { return _results && xQueueReceive(_results, &result, 0) == pdTRUE; }

private:
  struct Job { uint32_t generation; FileFormats::Format format; bool encrypted; char path[MaxPath]; };
  SecretStorage &_secretStorage;
  MediaCache &_mediaCache;
  QueueHandle_t _jobs;
  QueueHandle_t _results;
  TaskHandle_t _task;
  StorageFile _file;
  bool _encrypted;
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
      result.pixels = worker->decode(String(job.path), job.format, job.encrypted);
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

  uint16_t *decode(const String &path, FileFormats::Format format, bool encrypted) {
    reset();
    _encrypted = encrypted;
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
    _file = _secretStorage.open(path, FILE_READ, _encrypted);
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
    vTaskDelay(1);
    return got;
  }
  bool seekFile(uint32_t offset) {
    if (!_file || !SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
    bool ok = _file.seek(offset);
    SpiBusLock::unlock();
    vTaskDelay(1);
    return ok;
  }

  bool sourceInfo(const String &path) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
    StorageFile source = _secretStorage.open(path, FILE_READ, _encrypted);
    SpiBusLock::unlock();
    if (!source) return false;
    SourceFingerprint fingerprint{};
    const bool captured = _mediaCache.captureFingerprint(source, fingerprint) == MediaCache::Result::Complete;
    _mediaCache.close(source);
    if (captured) {
      _sourceSize = fingerprint.size;
      _sourceWriteTime = fingerprint.modified;
    }
    return captured;
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

  bool readCacheFile(StorageFile &file, uint8_t *data, size_t length) {
    return _mediaCache.read(file, data, length) == MediaCache::Result::Complete;
  }

  bool writeCacheFile(StorageFile &file, const uint8_t *data, size_t length) {
    return _mediaCache.write(file, data, length) == MediaCache::Result::Complete;
  }

  void closeCacheFile(StorageFile &file) {
    _mediaCache.close(file);
  }

  bool loadCache(const String &sourcePath, FileFormats::Format format) {
    CachePaths paths;
    if (!CachePath::forSource(sourcePath, FileFormats::Format::Eth1, paths)) return false;
    StorageFile cache;
    if (_mediaCache.openRead(paths, _encrypted, cache) != MediaCache::Result::Complete) return false;
    uint32_t size = 0;
    if (_mediaCache.fileSize(cache, size) != MediaCache::Result::Complete) {
      closeCacheFile(cache);
      return false;
    }
    if (!cache || size != CacheHeaderBytes + Size * Size * sizeof(uint16_t)) {
      closeCacheFile(cache);
      return false;
    }
    uint8_t header[CacheHeaderBytes] = {};
    const bool valid = readCacheFile(cache, header, sizeof(header)) &&
      memcmp(header, "ETH1", 4) == 0 && header[4] == 1 && header[5] == (uint8_t)format &&
      header[6] == Size && header[7] == Size &&
      MediaCache::matches({readLe32(header + 8), readLe32(header + 12)},
                          {_sourceSize, _sourceWriteTime}) &&
      readLe32(header + 16) == Size * Size * sizeof(uint16_t);
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
    CachePaths paths;
    if (!CachePath::forSource(sourcePath, FileFormats::Format::Eth1, paths)) return;
    MediaCache::Writer writer;
    if (_mediaCache.beginWrite(paths, _encrypted, writer) != MediaCache::Result::Complete) return;
    uint8_t header[CacheHeaderBytes] = {'E', 'T', 'H', '1', 1, (uint8_t)format, Size, Size};
    writeLe32(header + 8, _sourceSize);
    writeLe32(header + 12, _sourceWriteTime);
    writeLe32(header + 16, Size * Size * sizeof(uint16_t));
    bool written = _mediaCache.write(writer, header, sizeof(header)) == MediaCache::Result::Complete;
    const uint8_t *pixels = reinterpret_cast<const uint8_t *>(_pixels);
    for (size_t offset = 0; written && offset < Size * Size * sizeof(uint16_t); offset += 256) {
      const size_t amount = Size * Size * sizeof(uint16_t) - offset > 256 ? 256 : Size * Size * sizeof(uint16_t) - offset;
      written = _mediaCache.write(writer, pixels + offset, amount) == MediaCache::Result::Complete;
    }
    if (written) {
      if (_mediaCache.commit(writer) != MediaCache::Result::Complete) _mediaCache.abort(writer);
    } else {
      _mediaCache.abort(writer);
    }
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
    if (!_owner || !draw || draw->iWidth <= 0) return 0;
    // PNGdec expands packed indexed rows in groups of up to four pixels. Its
    // ESP32-S3 RGBA fast path likewise stores complete eight-pixel groups.
    // Keep tail room for that final group; only draw->iWidth pixels are read.
    uint16_t *line = static_cast<uint16_t *>(malloc(((size_t)draw->iWidth + 7) * sizeof(uint16_t)));
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
    const int width = _png.getWidth();
    const int height = _png.getHeight();
    const int bitsPerChannel = _png.getBpp();
    const int pixelType = _png.getPixelType();
    const int channels = pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED ? 1 :
                         pixelType == PNG_PIXEL_TRUECOLOR ? 3 :
                         pixelType == PNG_PIXEL_GRAY_ALPHA ? 2 :
                         pixelType == PNG_PIXEL_TRUECOLOR_ALPHA ? 4 : 0;
    const uint64_t scanlineBytes = channels > 0 && bitsPerChannel > 0
      ? ((uint64_t)width * (uint32_t)channels * (uint32_t)bitsPerChannel + 7) / 8 : 0;
    if (width <= 0 || height <= 0 || width > (int)MaxPngDimension || height > (int)MaxPngDimension ||
        scanlineBytes == 0 || scanlineBytes > MaxPngScanlineBytes) {
      _failure = F("PNG dimensions exceed thumbnail limit");
      _png.close();
      return false;
    }
    if (!prepare((uint16_t)width, (uint16_t)height, (uint16_t)width, (uint16_t)height)) { _png.close(); return false; }
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
                         SdCardController &sdCard,
                         SecretStorage &secretStorage,
                         MediaCache &mediaCache)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _secretStorage(secretStorage),
    _mediaCache(mediaCache),
    _currentPath("/"),
    _pendingRenamePath(),
    _pendingRenameDirectory(false),
    _renameSourcePath(),
    _renameTargetPath(),
    _message(),
    _messageUntilMs(0),
    _page(0),
    _sdMounted(false),
    _lastTouchDown(false),
    _retainCacheWhileCovered(false),
    _secretMode(false),
    _listing(nullptr),
    _thumbnailWorker(new FileThumbnailWorker(secretStorage, mediaCache)),
    _listingGeneration(1),
    _scanDirectory(),
    _scanCache(nullptr),
    _scanPhase(ScanPhase::Counting),
    _scanTotal(0),
    _scanProcessed(0),
    _scanVisibleCount(0),
    _showLoading(false),
    _lastLoadingPercent(0),
    _lastLoadingDeterminate(false),
    _loadingProgressDirty(false),
    _scanCancelPending(false),
    _listingDirty(false),
    _lastListingDrawMs(0),
    _rows(nullptr),
    _rowCapacity(0),
    _rowCount(0),
    _rowSlots(1),
    _pageCount(1),
    _parentBounds{0, 0, 0, 0},
    _previousBounds{0, 0, 0, 0},
    _nextBounds{0, 0, 0, 0},
    _menuBounds{0, 0, 0, 0},
    _renameBounds{0, 0, 0, 0},
    _moveBounds{0, 0, 0, 0},
    _deleteBounds{0, 0, 0, 0},
    _menuState(MenuState::None),
    _menuEntryIndex(InvalidEntryIndex),
    _deleteStartedMs(0),
    _lastDeleteDrawMs(0),
    _operationState(OperationState::None),
    _tasks(nullptr),
    _taskCount(0),
    _taskCapacity(0),
    _taskIndex(0),
    _moveSourcePath(),
    _moveTargetPath(),
    _moveStagePath(),
    _moveStagedPath(),
    _operationFailure(),
    _moveSource(),
    _moveTarget(),
    _moveBytes(0),
    _moveSourceDirectory(false),
    _moveStageCreated(false),
    _movePromoted(false),
    _moveCachesCleaned(false) {
  for (uint8_t i = 0; i < MaxCachedDirectories; i++) {
    _cache[i] = {"", nullptr, 0, 0, 0, false, false};
  }
  for (uint8_t i = 0; i < MaxCachedThumbnails; i++) {
    _thumbnails[i] = {"", nullptr, 0, 0, false, false, false};
  }
}

FilesScreen::~FilesScreen() {
  cancelDirectoryScan();
  closeDirectoryScan();
  if (_moveSource) _moveSource.close();
  if (_moveTarget) _moveTarget.close();
  clearTasks();
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
  _secretMode = false;
  _sdMounted = _sdCard.isMounted();
  if (_sdMounted && !ensureListing(_currentPath)) showMessage(F("Unable to list folder"));
  draw();
}

void FilesScreen::onEnter(const ScreenData &data) {
  onEnter();
  if (data.kind != ScreenDataKind::SecretFiles || !_sdMounted) return;
  String error;
  if (!_secretStorage.unlock(data.pin, error)) {
    showMessage(error.length() ? error : String(F("Secret storage unavailable")), 3000);
    draw();
    return;
  }
  _secretMode = true;
  clearCache();
  clearThumbnails();
  _listingGeneration++;
  if (!ensureListing(_currentPath)) showMessage(F("Unable to list folder"));
  draw();
}

void FilesScreen::onExit() {
  if (!_retainCacheWhileCovered) {
    clearCache();
    clearThumbnails();
    _pendingRenamePath = "";
    if (_secretMode) {
      _secretStorage.lock();
      _secretMode = false;
    }
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
  _pendingRenameDirectory = false;
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

  if (_operationState != OperationState::None) return;
  if (UiKit::navigationIconContains(point.x, point.y)) {
    _navigator.pop();
    return;
  }
  if (!_sdMounted) return;
  // An incremental sorted insert can move rows between display refreshes.
  // Bring the hit-test map in sync before interpreting this tap.
  if (_listingDirty) {
    redrawListingArea();
    _listingDirty = false;
    _lastListingDrawMs = millis();
    if (_menuState != MenuState::None) drawContextMenu();
  }
  if (_menuState != MenuState::None) {
    if (_renameBounds.contains(point.x, point.y)) {
      startRename();
    } else if (_moveBounds.contains(point.x, point.y)) {
      beginMoveToSecret();
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
  if (_parentBounds.contains(point.x, point.y)) {
    if (_currentPath != "/") openParentDirectory();
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
      if (entry && point.x >= _display.width() - ActionWidth && canActOnEntry(*entry)) showMenu(row.entryIndex);
      else openEntry(row.entryIndex);
    }
    return;
  }
}

void FilesScreen::tick(uint32_t nowMs) {
  handleSdCardEvent(_sdCard.takeEvent());
  updateDirectoryScan();
  if (_listingDirty && (uint32_t)(nowMs - _lastListingDrawMs) >= 50) {
    redrawListingArea();
    _listingDirty = false;
    _lastListingDrawMs = nowMs;
    if (_menuState != MenuState::None) drawContextMenu();
  }
  if (_loadingProgressDirty && !_listingDirty && _display.isReady()) {
    drawLoading(false);
    _loadingProgressDirty = false;
  }
  collectThumbnailResults();
  if (_operationState != OperationState::None) updateOperation();
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
  UiKit::drawTopAppBar(_display.tft(), _display.width(), F("Files"), UiKit::NavigationIcon::Close);
}

void FilesScreen::drawPathHeader() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(16, 56, _display.width() - 32, 24, TFT_DARKGREY);
  UiText::draw(tft, UiKit::ellipsize(_currentPath, _display.width() - 48, UiText::Size::Compact),
               24, 68, TFT_WHITE, TFT_DARKGREY, UiText::Size::Compact, ML_DATUM);
}

void FilesScreen::layoutPaging() {
  const int16_t contentBottom = _display.height() - FooterHeight;
  const int16_t height = contentBottom > RowTop ? contentBottom - RowTop : UiKit::ListCellHeight;
  _rowSlots = (uint8_t)(height / UiKit::ListCellHeight);
  if (_rowSlots == 0) _rowSlots = 1;
  ensureRowCapacity(_rowSlots);
  const int16_t quarter = _display.width() / 4;
  const int16_t footerTop = _display.height() - FooterHeight;
  _parentBounds = {0, footerTop, quarter, FooterHeight};
  _previousBounds = {(int16_t)(quarter * 2), footerTop, quarter, FooterHeight};
  _nextBounds = {(int16_t)(quarter * 3), footerTop, (int16_t)(_display.width() - quarter * 3), FooterHeight};
  updatePageCount();
}

uint16_t FilesScreen::firstEntryForPage(uint16_t page) const {
  return (uint32_t)page * entryCapacityForPage(page);
}

uint16_t FilesScreen::entryCapacityForPage(uint16_t page) const {
  (void)page;
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
  if (_listing && !_listing->complete) {
    if (_showLoading) drawLoading(true);
    return;
  }

  if (!_listing || _listing->count == 0) {
    const int16_t contentBottom = _display.height() - FooterHeight;
    UiText::draw(_display.tft(), F("Empty"), _display.width() / 2,
                 RowTop + (contentBottom - RowTop) / 2, TFT_LIGHTGREY, TFT_BLACK,
                 UiText::Size::Title, MC_DATUM);
    return;
  }

  auto addRow = [&](int16_t y, uint16_t index, bool parent) -> Row * {
    if (_rowCount >= _rowCapacity) return nullptr;
    Row &row = _rows[_rowCount++];
    row = {{0, y, _display.width(), UiKit::ListCellHeight}, index, parent};
    return &row;
  };

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

void FilesScreen::drawLoading(bool initial) {
  TFT_eSPI &tft = _display.tft();
  const int16_t width = _display.width() - 48;
  const int16_t x = 24;
  const int16_t y = RowTop + 62;
  const bool determinate = _scanPhase == ScanPhase::Listing && _scanTotal;
  const uint8_t percent = determinate
    ? (uint8_t)((uint32_t)_scanProcessed * 100 / _scanTotal) : 0;
  const String detail = determinate ? String(F("Indexing files ")) + String(percent) + F("%")
                                    : String(F("Counting files..."));

  if (initial) {
    UiText::draw(tft, F("Loading files"), _display.width() / 2, y - 42,
                 TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Title, MC_DATUM);
  }
  // Keep the static title and bar frame intact. Only this detail line and the
  // fill are repainted as indexing advances, matching Browser's no-flicker UI.
  tft.fillRect(0, y - 30, _display.width(), 20, TFT_BLACK);
  UiText::draw(tft, detail, _display.width() / 2, y - 20, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  if (determinate && _lastLoadingDeterminate && !initial) {
    UiKit::updateProgressBar(tft, x, y, width, 12, _lastLoadingPercent, percent,
                             TFT_BLUE, TFT_BLACK);
  } else {
    UiKit::drawProgressBar(tft, x, y, width, 12, percent, !determinate,
                           TFT_BLUE, TFT_DARKGREY, TFT_BLACK, true, millis());
  }
  _lastLoadingPercent = percent;
  _lastLoadingDeterminate = determinate;
  if (initial) _loadingProgressDirty = false;
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
  const bool secretDirectory = _secretMode && _currentPath == "/" && entry->directory &&
                               entry->storageName == F(".configs");
  const uint8_t *icon = entry->directory ? ic_dir : ic_file;
  uint16_t color = secretDirectory ? TFT_RED : FolderColor;
  if (!entry->directory) {
    const bool openable = FileFormats::isOpenable(entry->format);
    color = openable && FileFormats::isImage(entry->format) ? ImageFileColor
          : openable && FileFormats::isAudio(entry->format) ? AudioFileColor
          : openable && FileFormats::isVideo(entry->format) ? VideoFileColor : FileColor;
  }
  UiKit::drawListCell(_display.tft(), _display.width(), row.bounds.y, entry->name, icon, color,
                      canActOnEntry(*entry) ? String(F("            ")) : String());
  if (!entry->directory) {
    const String path = fullPathFor(*entry);
    Thumbnail *thumbnail = thumbnailFor(path);
    if (thumbnail && thumbnail->pixels) _display.tft().pushImage(8, row.bounds.y + 4, FileThumbnailWorker::Size,
                                                                  FileThumbnailWorker::Size, thumbnail->pixels);
  }
  if (canActOnEntry(*entry)) {
    UiKit::drawListMoreIcon(_display.tft(), _display.width() - ActionWidth / 2,
                            row.bounds.y + UiKit::ListCellHeight / 2,
                            TFT_LIGHTGREY, TFT_BLACK);
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
  const int16_t quarter = _display.width() / 4;
  const Rect emptyBounds = {quarter, footerTop, quarter, FooterHeight};
  const Rect buttons[] = {_parentBounds, emptyBounds, _previousBounds, _nextBounds};
  const bool active[] = {_currentPath != "/", false, _page > 0, _page + 1 < _pageCount};
  for (uint8_t i = 0; i < 4; i++) {
    const Rect &button = buttons[i];
    tft.fillRect(button.x, button.y, button.w - 1, button.h, FooterButtonColor);
  }
  tft.fillRect(_display.width() - 1, footerTop, 1, FooterHeight, TFT_DARKGREY);
  UiText::draw(tft, F("Page"), emptyBounds.x + emptyBounds.w / 2, footerTop + 12,
               TFT_LIGHTGREY, FooterButtonColor, UiText::Size::Compact, MC_DATUM);
  UiText::draw(tft, String(_page + 1) + F(" / ") + String(_pageCount),
               emptyBounds.x + emptyBounds.w / 2, footerTop + 29,
               TFT_WHITE, FooterButtonColor, UiText::Size::Compact, MC_DATUM);
  UiKit::drawBackIcon(tft, _parentBounds.x + _parentBounds.w / 2,
                      footerTop + FooterHeight / 2, active[0] ? TFT_WHITE : 0x8410,
                      FooterButtonColor);
  UiKit::drawPageArrow(tft, _previousBounds.x + _previousBounds.w / 2,
                       footerTop + FooterHeight / 2, true, false,
                       active[2] ? TFT_WHITE : 0x8410,
                       FooterButtonColor);
  UiKit::drawPageArrow(tft, _nextBounds.x + _nextBounds.w / 2,
                       footerTop + FooterHeight / 2, true, true,
                       active[3] ? TFT_WHITE : 0x8410,
                       FooterButtonColor);
}

void FilesScreen::drawContextMenu() {
  if (_menuEntryIndex == InvalidEntryIndex || !entryAt(_menuEntryIndex)) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t menuWidth = 150;
  const bool canMove = _secretMode && !_secretStorage.isSecretPath(_currentPath);
  const int16_t menuHeight = canMove ? 114 : 76;
  const int16_t x = _display.width() - menuWidth - 8;
  const int16_t y = RowTop + 4;
  _menuBounds = {x, y, menuWidth, menuHeight};
  const int16_t rowHeight = canMove ? menuHeight / 3 : menuHeight / 2;
  _renameBounds = {x, y, menuWidth, rowHeight};
  _moveBounds = canMove ? Rect{x, (int16_t)(y + rowHeight), menuWidth, rowHeight} : Rect{0, 0, 0, 0};
  _deleteBounds = {x, (int16_t)(y + rowHeight * (canMove ? 2 : 1)), menuWidth,
                   (int16_t)(menuHeight - rowHeight * (canMove ? 2 : 1))};
  tft.fillRoundRect(x, y, menuWidth, menuHeight, 5, MenuColor);
  tft.drawRoundRect(x, y, menuWidth, menuHeight, 5, TFT_LIGHTGREY);
  if (canMove) tft.drawFastHLine(x + 8, _moveBounds.y, menuWidth - 16, TFT_DARKGREY);
  tft.drawFastHLine(x + 8, _deleteBounds.y, menuWidth - 16, TFT_DARKGREY);
  const Entry *entry = entryAt(_menuEntryIndex);
  UiText::draw(tft, entry && entry->directory ? F("Rename folder") : F("Rename"),
               x + 14, _renameBounds.y + _renameBounds.h / 2, TFT_WHITE, MenuColor,
               UiText::Size::Compact, ML_DATUM);
  if (canMove) {
    UiText::draw(tft, F("Move to secret"), x + 14, _moveBounds.y + _moveBounds.h / 2,
                 TFT_WHITE, MenuColor, UiText::Size::Compact, ML_DATUM);
  }
  const bool holding = _menuState == MenuState::DeleteHolding;
  if (holding) {
    uint32_t elapsed = millis() - _deleteStartedMs;
    uint8_t percent = elapsed >= DeleteHoldMs ? 100 : (uint8_t)(elapsed * 100UL / DeleteHoldMs);
    tft.fillRect(x + 4, _deleteBounds.y + 3, (menuWidth - 8) * percent / 100, _deleteBounds.h - 6, DangerColor);
  }
  UiText::draw(tft, holding ? F("Keep holding...") : F("Hold to delete"), x + 14,
               _deleteBounds.y + _deleteBounds.h / 2, holding ? TFT_WHITE : TFT_RED,
               holding ? DangerColor : MenuColor, UiText::Size::Compact, ML_DATUM);
}

void FilesScreen::drawStatus() {
  if (_message.length() == 0) return;
  TFT_eSPI &tft = _display.tft();
  const Rect bounds = statusBounds();
  tft.fillRoundRect(bounds.x, bounds.y, bounds.w, bounds.h, 5, TFT_DARKGREY);
  UiText::draw(tft, UiKit::ellipsize(_message, bounds.w - 12, UiText::Size::Compact),
               _display.width() / 2, bounds.y + bounds.h / 2, TFT_WHITE, TFT_DARKGREY,
               UiText::Size::Compact, MC_DATUM);
}

void FilesScreen::drawUnavailable() {
  TFT_eSPI &tft = _display.tft();
  int16_t centerX = _display.width() / 2;
  tft.fillRect(centerX - 24, 116, 48, 48, TFT_DARKGREY);
  UiText::draw(tft, F("SD card unavailable"), centerX, 185, TFT_WHITE, TFT_BLACK,
               UiText::Size::Title, MC_DATUM);
  UiText::draw(tft, F("Insert or mount an SD card"), centerX, 218, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
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
    const bool encrypted = _secretMode && _secretStorage.isSecretPath(_currentPath);
    if (_thumbnailWorker->enqueue(path, entry->format, _listingGeneration, encrypted)) {
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
  // A scan only keeps an open directory handle for the currently visible
  // folder. Restart abandoned partial caches rather than risking duplicates.
  if (cached && !cached->complete && cached != _scanCache) {
    releaseCache(*cached);
    cached = nullptr;
  }
  if (!cached) {
    cached = allocateCacheSlot();
    if (!cached) return false;
    *cached = {path, nullptr, 0, 0, millis(), true, false};
    _listingGeneration++;
    _lastListingDrawMs = 0;
  }
  cached->lastUsed = millis();
  _listing = cached;
  updatePageCount();
  return true;
}

void FilesScreen::updateDirectoryScan() {
  // Directory enumeration is deliberately one entry at a time. The SD card
  // shares SPI with touch input, so a full blocking scan makes taps appear to
  // freeze the UI on folders with many files.
  if (_scanCancelPending) {
    closeDirectoryScan();
    return;
  }

  // Entry insertion shifts list indexes. Keep a selected context-menu entry
  // stable until its action is dismissed or completed.
  if (_menuState != MenuState::None) return;

  if (_scanCache && _scanCache != _listing) {
    cancelDirectoryScan();
    return;
  }
  if (!_scanCache && _listing && !_listing->complete) {
    _scanCache = _listing;
    _scanPhase = ScanPhase::Counting;
    _scanTotal = 0;
    _scanProcessed = 0;
    _scanVisibleCount = 0;
    _showLoading = false;
    _lastLoadingPercent = 0;
    _lastLoadingDeterminate = false;
    _loadingProgressDirty = false;
  }
  if (!_scanCache || _scanCache->complete) return;

  if (!_scanDirectory) {
    if (!SpiBusLock::lock(0)) return;
    _scanDirectory = SD.open(_scanCache->path.c_str());
    const bool valid = _scanDirectory && _scanDirectory.isDirectory();
    if (!valid && _scanDirectory) _scanDirectory.close();
    SpiBusLock::unlock();
    if (!valid) {
      _scanDirectory = File();
      _scanCache->complete = true;
      _scanCache = nullptr;
      showMessage(F("Unable to list folder"));
      _listingDirty = true;
    }
    return;
  }

  String path;
  bool directory = false;
  bool finished = false;
  if (!SpiBusLock::lock(0)) return;
  File entry = _scanDirectory.openNextFile();
  if (entry) {
    path = entry.name();
    directory = entry.isDirectory();
    entry.close();
  } else {
    _scanDirectory.close();
    _scanDirectory = File();
    finished = true;
  }
  SpiBusLock::unlock();

  if (finished) {
    if (_scanPhase == ScanPhase::Counting) {
      // A count gives the second pass an honest percentage without keeping
      // entries on screen while the folder is still changing underneath it.
      _scanPhase = ScanPhase::Listing;
      _scanProcessed = 0;
      if (_showLoading) _loadingProgressDirty = true;
    } else {
      _scanCache->complete = true;
      updatePageCount();
      _scanCache = nullptr;
      _showLoading = false;
      _loadingProgressDirty = false;
      _listingDirty = true;
    }
    return;
  }
  if (_scanPhase == ScanPhase::Counting) {
    if (_scanTotal < 0xfffe) _scanTotal++;
    if (!isHiddenEntry(path, _scanCache->path) && _scanVisibleCount < 0xfffe) {
      _scanVisibleCount++;
      // Avoid a distracting loading state for directories that are small
      // enough to finish before the user needs progress feedback.
      if (_scanVisibleCount == 10) {
        _showLoading = true;
        _listingDirty = true;
      }
    }
  } else {
    if (_scanProcessed < _scanTotal) _scanProcessed++;
    if (!isHiddenEntry(path, _scanCache->path) && appendDirectoryEntry(*_scanCache, path, directory)) {
      _scanCache->lastUsed = millis();
    }
    const uint8_t percent = _scanTotal
      ? (uint8_t)((uint32_t)_scanProcessed * 100 / _scanTotal) : 0;
    if (_showLoading && (!_lastLoadingDeterminate || percent != _lastLoadingPercent)) {
      _loadingProgressDirty = true;
    }
  }
}

void FilesScreen::cancelDirectoryScan() {
  _scanCache = nullptr;
  if (_scanDirectory) {
    _scanCancelPending = true;
    closeDirectoryScan();
  }
}

void FilesScreen::closeDirectoryScan() {
  if (!_scanDirectory) {
    _scanCancelPending = false;
    return;
  }
  if (!SpiBusLock::lock(0)) return;
  _scanDirectory.close();
  _scanDirectory = File();
  SpiBusLock::unlock();
  _scanCancelPending = false;
}

bool FilesScreen::appendDirectoryEntry(DirectoryCache &cache, const String &storagePath, bool directory) {
  if (cache.count >= 0xfffe) return false;
  if (cache.count == cache.capacity) {
    uint16_t capacity = cache.capacity ? (uint16_t)(cache.capacity * 2) : 8;
    if (capacity < cache.capacity || capacity > 0xfffe) capacity = 0xfffe;
    Entry *entries = new Entry[capacity];
    if (!entries) {
      showMessage(F("Folder list is out of memory"));
      cache.complete = true;
      cancelDirectoryScan();
      return false;
    }
    for (uint16_t i = 0; i < cache.count; i++) entries[i] = cache.entries[i];
    delete[] cache.entries;
    cache.entries = entries;
    cache.capacity = capacity;
  }

  const String stored = displayName(storagePath);
  String decrypted = stored;
  if (_secretMode && _secretStorage.isSecretPath(cache.path) && !_secretStorage.decryptName(stored, decrypted)) {
    decrypted = "?";
  }
  Entry next;
  next.name = _secretMode && _secretStorage.isSecretPath(cache.path)
    ? _secretStorage.displayName(decrypted) : decrypted;
  next.storageName = stored;
  if (_secretMode && cache.path == "/" && stored == String(SecretStorage::Directory).substring(1)) {
    next.name = F("secret");
  }
  next.directory = directory;
  next.format = directory ? FileFormats::Format::Unknown : FileFormats::fromPath(decrypted);

  // Inserting each discovered entry keeps partial results in the same natural
  // order as a completed directory, without a final long sort pause.
  uint16_t index = cache.count;
  while (index > 0 && compareEntries(next, cache.entries[index - 1]) < 0) {
    cache.entries[index] = cache.entries[index - 1];
    index--;
  }
  cache.entries[index] = next;
  cache.count++;
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
  if (&cache == _scanCache) cancelDirectoryScan();
  delete[] cache.entries;
  cache = {"", nullptr, 0, 0, 0, false, false};
  if (&cache == _listing) _listing = nullptr;
}

void FilesScreen::clearCache() {
  cancelDirectoryScan();
  for (uint8_t i = 0; i < MaxCachedDirectories; i++) releaseCache(_cache[i]);
  _listing = nullptr;
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

bool FilesScreen::isHiddenEntry(const String &path, const String &directory) const {
  const String name = displayName(path);
  if (name.startsWith(F("._")) || name == F(".cache") || name.endsWith(F(".tmp"))) return true;
  if (name == F(".configs")) return !_secretMode;
  return directory == SecretStorage::Directory && name == F("index");
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
  const String &name = entry.storageName.length() ? entry.storageName : entry.name;
  return _currentPath == "/" ? "/" + name : _currentPath + "/" + name;
}

String FilesScreen::displayName(const String &path) const {
  int16_t slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(slash + 1);
}

bool FilesScreen::canActOnEntry(const Entry &entry) const {
  if (_secretMode && _secretStorage.isSecretPath(_currentPath)) return false;
  return !(_currentPath == "/" && entry.directory &&
           entry.storageName == String(SecretStorage::Directory).substring(1));
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
  const bool secret = _secretMode && _secretStorage.isSecretPath(_currentPath);
  if (FileFormats::isImage(entry->format) && FileFormats::isOpenable(entry->format)) {
    ScreenData imageData;
    if (!imageViewerDataFor(path, imageData)) {
      imageData = secret ? ScreenData::secretImagePath(path, _secretStorage.pin()) : ScreenData::imagePath(path);
    }
    opened = _navigator.push(ScreenId::ImageViewer, imageData);
  } else if (FileFormats::isAudio(entry->format) && FileFormats::isOpenable(entry->format)) {
    opened = _navigator.push(ScreenId::AudioPlayer, secret ? ScreenData::secretAudioPath(path, _secretStorage.pin()) : ScreenData::audioPath(path));
  } else if (FileFormats::isVideo(entry->format) && FileFormats::isOpenable(entry->format)) {
    opened = _navigator.push(ScreenId::VideoPlayer, secret ? ScreenData::secretVideoPath(path, _secretStorage.pin()) : ScreenData::videoPath(path));
  }
  if (!opened) {
    _retainCacheWhileCovered = false;
  }
}

bool FilesScreen::imageViewerDataFor(const String &path, ScreenData &data) const {
  if (!_listing || _listing->count == 0) return false;

  uint16_t current = InvalidEntryIndex;
  for (uint16_t i = 0; i < _listing->count; i++) {
    const Entry &entry = _listing->entries[i];
    if (!entry.directory && FileFormats::isImage(entry.format) && FileFormats::isOpenable(entry.format) &&
        fullPathFor(entry) == path) {
      current = i;
      break;
    }
  }
  if (current == InvalidEntryIndex) return false;

  String previous;
  String next;
  for (uint16_t step = 1; step < _listing->count && (previous.length() == 0 || next.length() == 0); step++) {
    const uint16_t previousIndex = (uint16_t)((current + _listing->count - step) % _listing->count);
    const uint16_t nextIndex = (uint16_t)((current + step) % _listing->count);
    const Entry &previousEntry = _listing->entries[previousIndex];
    const Entry &nextEntry = _listing->entries[nextIndex];
    if (previous.length() == 0 && !previousEntry.directory && FileFormats::isImage(previousEntry.format) &&
        FileFormats::isOpenable(previousEntry.format)) {
      previous = fullPathFor(previousEntry);
    }
    if (next.length() == 0 && !nextEntry.directory && FileFormats::isImage(nextEntry.format) &&
        FileFormats::isOpenable(nextEntry.format)) {
      next = fullPathFor(nextEntry);
    }
  }

  const bool secret = _secretMode && _secretStorage.isSecretPath(_currentPath);
  data = secret ? ScreenData::secretImagePath(path, _secretStorage.pin(), previous, next)
                : ScreenData::imagePath(path, previous, next);
  return true;
}

void FilesScreen::showMenu(uint16_t index) {
  Entry *entry = entryAt(index);
  if (!entry || !canActOnEntry(*entry)) return;
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
  _pendingRenameDirectory = entry->directory;
  _retainCacheWhileCovered = true;
  closeMenu();
  if (!_navigator.push(ScreenId::TextInput,
                       ScreenData::textInput(entry->directory ? "Rename folder" : "Rename file", false, entry->name))) {
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
  if (!_pendingRenameDirectory) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
      showMessage(F("Storage busy; rename not started"));
      draw();
      return;
    }
    const bool targetExists = SD.exists(target.c_str());
    SpiBusLock::unlock();
    if (targetExists) {
      showMessage(F("Rename failed or target exists"));
      draw();
      return;
    }
    _renameSourcePath = _pendingRenamePath;
    _renameTargetPath = target;
    _mediaCache.beginCleanup(_renameSourcePath, _cacheCleanup);
    _operationState = OperationState::RenameCleanup;
    showMessage(F("Preparing rename..."), 60000);
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
  showMessage(F("Renamed"));
  refreshListing();
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
    startDeleteSelected();
    return;
  }
  if (nowMs - _lastDeleteDrawMs >= 80) {
    _lastDeleteDrawMs = nowMs;
    drawContextMenu();
  }
}

void FilesScreen::startDeleteSelected() {
  Entry *entry = entryAt(_menuEntryIndex);
  if (!entry || !canActOnEntry(*entry)) {
    closeMenu();
    return;
  }
  const String path = fullPathFor(*entry);
  const bool directory = entry->directory;
  closeMenu();
  clearTasks();
  if (!appendTask(path, "", directory)) {
    showMessage(F("Not enough memory to delete"));
    draw();
    return;
  }
  if (directory) {
    _operationState = OperationState::Deleting;
  } else {
    _mediaCache.beginCleanup(path, _cacheCleanup);
    _operationState = OperationState::DeleteCacheCleanup;
  }
  showMessage(directory ? F("Deleting folder...") : F("Deleting..."), 60000);
  draw();
}

void FilesScreen::beginMoveToSecret() {
  Entry *entry = entryAt(_menuEntryIndex);
  if (!entry || !canActOnEntry(*entry) || !_secretMode || _secretStorage.isSecretPath(_currentPath) ||
      !_secretStorage.unlocked()) return;
  closeMenu();
  if (!prepareMoveTarget(*entry)) {
    draw();
    return;
  }
  _operationState = OperationState::MovePreflight;
  showMessage(entry->directory ? F("Checking folder...") : F("Checking file..."), 60000);
  draw();
}

bool FilesScreen::prepareMoveTarget(const Entry &entry) {
  String encoded;
  String error;
  if (!_secretStorage.encryptName(entry.name, encoded, error)) {
    showMessage(error);
    return false;
  }
  _moveSourcePath = fullPathFor(entry);
  _moveTargetPath = String(SecretStorage::Directory) + "/" + encoded;
  const uint32_t started = millis();
  uint8_t attempt = 0;
  do {
    _moveStagePath = String(SecretStorage::Directory) + F("/.move-") + String(started) + F("-") + String(attempt++) + F(".tmp");
  } while (_secretStorage.exists(_moveStagePath) && attempt != 0);
  if (_secretStorage.exists(_moveStagePath)) {
    showMessage(F("Secret staging area is busy"));
    return false;
  }
  _moveStagedPath = _moveStagePath + "/" + encoded;
  _moveSourceDirectory = entry.directory;
  _moveStageCreated = false;
  _movePromoted = false;
  _moveCachesCleaned = false;
  _cacheCleanup = MediaCache::Cleanup();
  _moveBytes = 0;
  _operationFailure = "";
  if (_secretStorage.exists(_moveTargetPath)) {
    showMessage(entry.directory ? F("Secret folder already exists") : F("Secret file already exists"));
    return false;
  }
  clearTasks();
  _taskIndex = 0;
  if (!appendTask(_moveSourcePath, _moveStagedPath, entry.directory)) {
    showMessage(F("Not enough memory to move"));
    return false;
  }
  return true;
}

void FilesScreen::updateOperation() {
  if (_operationState == OperationState::RenameCleanup ||
      _operationState == OperationState::DeleteCacheCleanup) {
    updateCacheCleanup();
  } else if (_operationState == OperationState::Deleting ||
      _operationState == OperationState::MoveRemoveSource ||
      _operationState == OperationState::MoveCleanupStage) {
    updateRecursiveDelete();
  } else if (_operationState == OperationState::MovePreflight) {
    updateMovePreflight();
  } else if (_operationState == OperationState::MoveCopying) {
    updateMoveCopy();
  } else if (_operationState == OperationState::MoveCommit) {
    updateMoveCommit();
  }
}

void FilesScreen::updateCacheCleanup() {
  const MediaCache::Result result = _mediaCache.cleanupStep(_cacheCleanup);
  if (result == MediaCache::Result::Busy || result == MediaCache::Result::More) return;
  if (result != MediaCache::Result::Complete) {
    finishOperation(false, _operationState == OperationState::RenameCleanup
      ? String(F("Rename stopped; cache cleanup failed"))
      : String(F("Delete stopped; cache cleanup failed")));
    return;
  }
  if (_operationState == OperationState::DeleteCacheCleanup) {
    _operationState = OperationState::Deleting;
    return;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
  const bool renamed = !SD.exists(_renameTargetPath.c_str()) &&
                       SD.rename(_renameSourcePath.c_str(), _renameTargetPath.c_str());
  SpiBusLock::unlock();
  finishOperation(renamed, renamed ? String(F("Renamed"))
                                   : String(F("Rename failed or target exists")));
}

void FilesScreen::updateMovePreflight() {
  if (_taskIndex >= _taskCount) {
    _taskIndex = 0;
    _operationState = OperationState::MoveCopying;
    showMessage(_moveSourceDirectory ? F("Moving folder to secret...") : F("Moving to secret..."), 60000);
    return;
  }
  const PathTask task = _tasks[_taskIndex++];
  if (!task.directory) return;
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
  File directory = SD.open(task.source.c_str(), FILE_READ);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    SpiBusLock::unlock();
    failMove(F("Cannot read source folder"));
    return;
  }
  bool valid = true;
  String error;
  File child = directory.openNextFile();
  while (child && valid) {
    const String name = displayName(child.name());
    if (name != F(".cache")) {
      String encoded;
      if (!_secretStorage.encryptName(name, encoded, error)) {
        valid = false;
      } else {
        String source = child.name();
        if (!source.startsWith("/")) source = task.source + "/" + name;
        valid = appendTask(source, task.target + "/" + encoded, child.isDirectory());
        if (!valid) error = F("Not enough memory to move folder");
      }
    }
    child.close();
    if (valid) child = directory.openNextFile();
  }
  directory.close();
  SpiBusLock::unlock();
  if (!valid) failMove(error.length() ? error : String(F("Cannot inspect source folder")));
}

void FilesScreen::updateMoveCopy() {
  if (!_moveStageCreated) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
    const bool ready = _secretStorage.mkdir(_moveStagePath);
    SpiBusLock::unlock();
    if (!ready) {
      failMove(F("Cannot prepare secret storage"));
      return;
    }
    _moveStageCreated = true;
  }
  if (_moveSource || _moveTarget) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return;
    const int count = _moveSource.read(_moveBuffer, sizeof(_moveBuffer));
    const bool written = count <= 0 || _moveTarget.write(_moveBuffer, (size_t)count) == (size_t)count;
    SpiBusLock::unlock();
    if (count < 0 || !written) {
      failMove(F("Secret move write failed"));
      return;
    }
    _moveBytes += count > 0 ? (uint32_t)count : 0;
    if (count > 0) return;
    _moveSource.close();
    _moveTarget.flush();
    _moveTarget.close();
    _taskIndex++;
    return;
  }
  if (_taskIndex >= _taskCount) {
    _operationState = OperationState::MoveCommit;
    return;
  }
  const PathTask &task = _tasks[_taskIndex];
  if (task.directory) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
    const bool made = _secretStorage.mkdir(task.target);
    SpiBusLock::unlock();
    if (!made) {
      failMove(F("Cannot create secret folder"));
      return;
    }
    _taskIndex++;
    return;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
  _moveSource = SD.open(task.source.c_str(), FILE_READ);
  _moveTarget = _secretStorage.open(task.target, FILE_WRITE, true);
  SpiBusLock::unlock();
  if (!_moveSource || !_moveTarget) {
    failMove(F("Cannot copy source file"));
  }
}

void FilesScreen::updateMoveCommit() {
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
  const bool committed = _secretStorage.rename(_moveStagedPath, _moveTargetPath);
  if (committed) SD.rmdir(_moveStagePath.c_str());
  SpiBusLock::unlock();
  if (!committed) {
    failMove(F("Secret move commit failed"));
    return;
  }
  _movePromoted = true;
  _moveStageCreated = false;
  _operationState = OperationState::MoveRemoveSource;
  clearTasks();
  _taskIndex = 0;
  showMessage(F("Removing original..."), 60000);
}

void FilesScreen::updateRecursiveDelete() {
  if (_operationState == OperationState::MoveRemoveSource && !_moveCachesCleaned) {
    if (!_moveSourceDirectory) {
      if (!_cacheCleanup.active && _cacheCleanup.sourcePath != _moveSourcePath) {
        _mediaCache.beginCleanup(_moveSourcePath, _cacheCleanup);
      }
      const MediaCache::Result cleanup = _mediaCache.cleanupStep(_cacheCleanup);
      if (cleanup == MediaCache::Result::Busy || cleanup == MediaCache::Result::More) return;
      if (cleanup != MediaCache::Result::Complete) {
        finishOperation(false, F("Secret copy kept; cache cleanup failed"));
        return;
      }
    }
    _moveCachesCleaned = true;
    if (!appendTask(_moveSourcePath, "", _moveSourceDirectory)) {
      finishOperation(false, F("Secret copy kept; cleanup ran out of memory"));
      return;
    }
  }
  if (_taskCount == 0) {
    if (_operationState == OperationState::Deleting) {
      finishOperation(true, F("Deleted"));
    } else if (_operationState == OperationState::MoveCleanupStage) {
      finishOperation(false, _operationFailure.length() ? _operationFailure : String(F("Secret move failed")));
    } else {
      finishOperation(true, F("Moved to secret"));
    }
    return;
  }
  const PathTask task = _tasks[_taskCount - 1];
  _taskCount--;
  if (task.directory && !task.expanded) {
    if (!appendTask(task.source, task.target, true, true) || !expandDeleteDirectory(task)) {
      if (_operationState == OperationState::Deleting) {
        finishOperation(false, F("Delete failed"));
      } else if (_operationState == OperationState::MoveCleanupStage) {
        finishOperation(false, _operationFailure.length() ? _operationFailure : String(F("Secret move failed")));
      } else {
        finishOperation(false, F("Secret copy kept; source cleanup failed"));
      }
    }
    return;
  }
  if (!removePath(task.source, task.directory)) {
    if (_operationState == OperationState::Deleting) {
      finishOperation(false, F("Delete failed"));
    } else if (_operationState == OperationState::MoveCleanupStage) {
      finishOperation(false, _operationFailure.length() ? _operationFailure : String(F("Secret move failed")));
    } else {
      finishOperation(false, F("Secret copy kept; source cleanup failed"));
    }
  }
}

bool FilesScreen::expandDeleteDirectory(const PathTask &task) {
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  File directory = SD.open(task.source.c_str(), FILE_READ);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    SpiBusLock::unlock();
    return false;
  }
  bool listed = true;
  File child = directory.openNextFile();
  while (child && listed) {
    String path = child.name();
    if (!path.startsWith("/")) path = task.source + "/" + displayName(path);
    listed = appendTask(path, "", child.isDirectory());
    child.close();
    if (listed) child = directory.openNextFile();
  }
  directory.close();
  SpiBusLock::unlock();
  return listed;
}

bool FilesScreen::removePath(const String &path, bool directory) {
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  const bool removed = directory ? SD.rmdir(path.c_str()) : SD.remove(path.c_str());
  SpiBusLock::unlock();
  return removed;
}

void FilesScreen::clearTasks() {
  delete[] _tasks;
  _tasks = nullptr;
  _taskCount = 0;
  _taskCapacity = 0;
  _taskIndex = 0;
}

bool FilesScreen::appendTask(const String &source, const String &target, bool directory, bool expanded) {
  if (_taskCount == 0xffff) return false;
  if (_taskCount == _taskCapacity) {
    uint16_t capacity = _taskCapacity ? (uint16_t)(_taskCapacity * 2) : 16;
    if (capacity <= _taskCapacity) capacity = 0xffff;
    PathTask *tasks = new PathTask[capacity];
    if (!tasks) return false;
    for (uint16_t i = 0; i < _taskCount; i++) tasks[i] = _tasks[i];
    delete[] _tasks;
    _tasks = tasks;
    _taskCapacity = capacity;
  }
  _tasks[_taskCount++] = {source, target, directory, expanded};
  return true;
}

void FilesScreen::failMove(const String &message) {
  if (_moveSource) _moveSource.close();
  if (_moveTarget) _moveTarget.close();
  _operationFailure = message;
  clearTasks();
  if (_moveStageCreated && !_movePromoted) {
    _operationState = OperationState::MoveCleanupStage;
    if (appendTask(_moveStagePath, "", true)) return;
  }
  finishOperation(false, message);
}

void FilesScreen::refreshListing() {
  clearCache();
  clearThumbnails();
  _listingGeneration++;
  if (_sdMounted && !ensureListing(_currentPath)) showMessage(F("Unable to list folder"));
  draw();
}

void FilesScreen::finishOperation(bool success, const String &message) {
  (void)success;
  if (_moveSource) _moveSource.close();
  if (_moveTarget) _moveTarget.close();
  _operationState = OperationState::None;
  clearTasks();
  _moveSourcePath = "";
  _moveTargetPath = "";
  _moveStagePath = "";
  _moveStagedPath = "";
  _operationFailure = "";
  _moveBytes = 0;
  _moveSourceDirectory = false;
  _moveStageCreated = false;
  _movePromoted = false;
  _moveCachesCleaned = false;
  _renameSourcePath = "";
  _renameTargetPath = "";
  _cacheCleanup = MediaCache::Cleanup();
  showMessage(message);
  refreshListing();
}

void FilesScreen::showMessage(const String &message, uint32_t durationMs) {
  _message = message;
  _messageUntilMs = millis() + durationMs;
}

void FilesScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::None) return;
  if ((event == SdCardEvent::Removed || event == SdCardEvent::MountFailed) && _operationState != OperationState::None) {
    if (_moveSource) _moveSource.close();
    if (_moveTarget) _moveTarget.close();
    _operationState = OperationState::None;
    clearTasks();
    _moveSourcePath = "";
    _moveTargetPath = "";
    _moveStagePath = "";
    _moveStagedPath = "";
    _operationFailure = "";
    _moveBytes = 0;
    _moveSourceDirectory = false;
    _moveStageCreated = false;
    _movePromoted = false;
    _moveCachesCleaned = false;
    _renameSourcePath = "";
    _renameTargetPath = "";
    _cacheCleanup = MediaCache::Cleanup();
    showMessage(F("SD card removed; operation stopped"), 3000);
  }
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
    _secretStorage.lock();
    _secretMode = false;
    clearCache();
    clearThumbnails();
    _listingGeneration++;
  }
  draw();
}
