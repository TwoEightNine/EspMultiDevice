#include "ImageViewerScreen.h"

#include "Navigator.h"

#include <SD.h>
#include <esp32-hal-psram.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {
const uint32_t InvalidSourceRow = 0xffffffffUL;
const uint32_t MaxSupportedDibSize = 124;
}

ImageViewerScreen *ImageViewerScreen::_decoderOwner = nullptr;

ImageViewerScreen::ImageViewerScreen(Navigator &navigator,
                                     TouchController &touch,
                                     DisplayController &display,
                                     SdCardController &sdCard)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _sourceFileSize(0),
    _sourceWriteTime(0),
    _cacheOffset(0),
    _format(ImageFormat::Unknown),
    _pixelOffset(0),
    _sourceWidth(0),
    _sourceHeight(0),
    _rowStride(0),
    _dibSize(0),
    _bitsPerPixel(0),
    _compression(Compression::Rgb),
    _topDown(false),
    _hasAlpha(false),
    _rle(false),
    _redMask(0),
    _greenMask(0),
    _blueMask(0),
    _alphaMask(0),
    _redShift(0),
    _greenShift(0),
    _blueShift(0),
    _alphaShift(0),
    _paletteCount(0),
    _left(0),
    _top(0),
    _drawWidth(0),
    _drawHeight(0),
    _nextDrawY(-1),
    _lastSourceY(InvalidSourceRow),
    _frameBuffer(nullptr),
    _xCoordinates(nullptr),
    _rawRow(nullptr),
    _row0(nullptr),
    _row1(nullptr),
    _rendering(false),
    _cacheWriting(false),
    _lastTouchDown(false),
    _lastProgress(0),
    _rleFileRow(0),
    _rleX(0),
    _rlePendingBlankRows(0),
    _rleDone(false) {
}

void ImageViewerScreen::onEnter(const ScreenData &data) {
  reset();
  _lastTouchDown = false;
  if (data.kind != ScreenDataKind::ImagePath || data.text.length() == 0) {
    _error = F("No image selected");
    drawError();
    return;
  }
  _path = data.text;
  if (!_sdCard.isMounted()) {
    _error = F("SD card unavailable");
    drawError();
    return;
  }
  _file = SD.open(_path.c_str(), FILE_READ);
  if (!_file) {
    _error = F("Unable to open source image");
    drawError();
    return;
  }
  _sourceFileSize = (uint32_t)_file.size();
  _sourceWriteTime = (uint32_t)_file.getLastWrite();
  _cachePath = cachePathForSource();
  _cacheTempPath = _cachePath + F(".tmp");
  if (loadCache()) {
    Serial.print(F("Image cache hit: "));
    Serial.println(_cachePath);
    draw();
    _display.tft().pushImage(0, 0, _display.width(), _display.height(), _frameBuffer);
    releaseBuffers();
    drawCloseIcon();
    return;
  }
  if (!readRasterHeader() || !prepareRender()) {
    if (_error.length() == 0) _error = F("Unable to render image");
    drawError();
    return;
  }
  draw();
  drawLoading(0);
  if (_format != ImageFormat::Bmp && !decodeRasterImage()) {
    if (_error.length() == 0) _error = F("Could not decode image");
    releaseBuffers();
    drawError();
    return;
  }
  _rendering = _format == ImageFormat::Bmp;
  if (!_rendering) {
    _display.tft().pushImage(0, 0, _display.width(), _display.height(), _frameBuffer);
    startCacheWrite();
    if (!_cacheWriting) releaseBuffers();
    drawCloseIcon();
    return;
  }
  if (!renderRows(16)) {
    _rendering = false;
    if (_error.length() == 0) _error = F("Could not read image data");
    releaseBuffers();
    drawError();
    return;
  }
  drawCloseIcon();
}

void ImageViewerScreen::onExit() {
  reset();
}

void ImageViewerScreen::tick(uint32_t nowMs) {
  (void)nowMs;
  if (_rendering && !renderRows(16)) {
    _rendering = false;
    if (_error.length() == 0) _error = F("Could not read image data");
    releaseBuffers();
    drawError();
  }
  if (_cacheWriting) writeCacheChunk();
}

void ImageViewerScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;
  if (point.x < CloseTouchSize && point.y < CloseTouchSize) _navigator.pop();
}

void ImageViewerScreen::reset() {
  _rendering = false;
  _decoderOwner = nullptr;
  if (_file) _file.close();
  if (_decoderFile) _decoderFile.close();
  if (_cacheFile) _cacheFile.close();
  if (_cacheWriting && _cacheTempPath.length() > 0) SD.remove(_cacheTempPath);
  _cacheWriting = false;
  releaseBuffers();
  _path = "";
  _error = "";
  _cachePath = "";
  _cacheTempPath = "";
  _sourceFileSize = 0;
  _sourceWriteTime = 0;
  _cacheOffset = 0;
  _format = ImageFormat::Unknown;
  _pixelOffset = 0;
  _sourceWidth = 0;
  _sourceHeight = 0;
  _rowStride = 0;
  _dibSize = 0;
  _bitsPerPixel = 0;
  _compression = Compression::Rgb;
  _topDown = false;
  _hasAlpha = false;
  _rle = false;
  _paletteCount = 0;
  _nextDrawY = -1;
  _lastSourceY = InvalidSourceRow;
  _lastProgress = 0;
  _rleFileRow = 0;
  _rleX = 0;
  _rlePendingBlankRows = 0;
  _rleDone = false;
}

void ImageViewerScreen::draw() {
  if (_display.isReady()) _display.tft().fillScreen(TFT_BLACK);
}

void ImageViewerScreen::drawCloseIcon() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillRoundRect(2, 2, 52, 52, 8, TFT_BLACK);
  for (int16_t offset = 0; offset < 4; offset++) {
    tft.drawLine(12 + offset, 12, 44 + offset, 44, TFT_WHITE);
    tft.drawLine(44 + offset, 12, 12 + offset, 44, TFT_WHITE);
  }
}

void ImageViewerScreen::drawError() {
  draw();
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  int16_t centerX = _display.width() / 2;
  int16_t centerY = _display.height() / 2;
  tft.drawString(F("Image unavailable"), centerX, centerY - 40, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(_error, centerX, centerY - 12, 2);
  String details = String(F("Format: ")) + formatName() + F("  Size: ") +
                   String(_sourceWidth) + F("x") + String(_sourceHeight);
  tft.drawString(details, centerX, centerY + 12, 2);
  String filename = _path;
  int slash = filename.lastIndexOf('/');
  if (slash >= 0) filename = filename.substring(slash + 1);
  if (filename.length() > 28) filename = String(F("...")) + filename.substring(filename.length() - 25);
  tft.drawString(filename, centerX, centerY + 36, 2);
  tft.setTextDatum(TL_DATUM);
  Serial.print(F("Image viewer error: format="));
  Serial.print(formatName());
  Serial.print(F(" size="));
  Serial.print(_sourceWidth);
  Serial.print('x');
  Serial.print(_sourceHeight);
  Serial.print(F(" file="));
  Serial.print(_path);
  Serial.print(F(" reason="));
  Serial.println(_error);
  drawCloseIcon();
}

const char *ImageViewerScreen::formatName() const {
  switch (_format) {
    case ImageFormat::Bmp: return "BMP";
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Jpeg: return "JPEG";
    default: return "unknown";
  }
}

void ImageViewerScreen::drawLoading(uint8_t percent) {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  int16_t centerX = _display.width() / 2;
  int16_t centerY = _display.height() / 2;
  tft.fillRect(centerX - 100, centerY - 28, 200, 58, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(F("Loading image"), centerX, centerY - 12, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  String progress = String(percent) + F("%");
  tft.drawString(progress, centerX, centerY + 14, 2);
  tft.setTextDatum(TL_DATUM);
  drawCloseIcon();
}

void ImageViewerScreen::updateDecodeProgress(uint8_t percent) {
  if (percent == 100 || percent >= (uint8_t)(_lastProgress + 5)) {
    _lastProgress = percent;
    drawLoading(percent);
  }
}

bool ImageViewerScreen::readHeader() {
  uint8_t fileHeader[14];
  if (!_file.seek(0) || _file.read(fileHeader, sizeof(fileHeader)) != sizeof(fileHeader) ||
      fileHeader[0] != 'B' || fileHeader[1] != 'M') {
    _error = F("Invalid BMP header");
    return false;
  }
  _pixelOffset = readLe32(fileHeader + 10);

  uint8_t dib[MaxSupportedDibSize];
  if (_file.read(dib, 4) != 4) {
    _error = F("Missing BMP DIB header");
    return false;
  }
  _dibSize = readLe32(dib);
  if (_dibSize < 40 || _dibSize > MaxSupportedDibSize ||
      _file.read(dib + 4, _dibSize - 4) != _dibSize - 4) {
    _error = F("Unsupported BMP header");
    return false;
  }

  int32_t width = readLe32Signed(dib + 4);
  int32_t height = readLe32Signed(dib + 8);
  uint16_t planes = readLe16(dib + 12);
  _bitsPerPixel = readLe16(dib + 14);
  uint32_t compressionValue = readLe32(dib + 16);
  uint32_t colorsUsed = readLe32(dib + 32);
  if (width <= 0 || height == 0 || height == INT32_MIN || planes != 1 ||
      (uint32_t)width > 0x1fffffffUL) {
    _error = F("Invalid BMP dimensions");
    return false;
  }

  _sourceWidth = (uint32_t)width;
  _topDown = height < 0;
  _sourceHeight = _topDown ? (uint32_t)(-height) : (uint32_t)height;
  _compression = (Compression)compressionValue;
  _rle = _compression == Compression::Rle4 || _compression == Compression::Rle8;
  if (_rle && _topDown) {
    _error = F("Top-down RLE BMP unsupported");
    return false;
  }
  if (_bitsPerPixel != 1 && _bitsPerPixel != 4 && _bitsPerPixel != 8 &&
      _bitsPerPixel != 16 && _bitsPerPixel != 24 && _bitsPerPixel != 32) {
    _error = F("Unsupported BMP pixel depth");
    return false;
  }
  if ((_compression == Compression::Rle4 && _bitsPerPixel != 4) ||
      (_compression == Compression::Rle8 && _bitsPerPixel != 8) ||
      (_rle && colorsUsed > MaxPaletteEntries)) {
    _error = F("Invalid BMP compression");
    return false;
  }
  if (!_rle && _compression != Compression::Rgb &&
      _compression != Compression::Bitfields && _compression != Compression::AlphaBitfields) {
    _error = F("Unsupported BMP compression");
    return false;
  }
  if ((_compression == Compression::Bitfields || _compression == Compression::AlphaBitfields) &&
      _bitsPerPixel != 16 && _bitsPerPixel != 32) {
    _error = F("Invalid BMP bitfields");
    return false;
  }

  uint32_t extraMasks = 0;
  if (_bitsPerPixel == 16 || _bitsPerPixel == 32) {
    if (_compression == Compression::Rgb) {
      _redMask = _bitsPerPixel == 16 ? 0x7c00UL : 0x00ff0000UL;
      _greenMask = _bitsPerPixel == 16 ? 0x03e0UL : 0x0000ff00UL;
      _blueMask = _bitsPerPixel == 16 ? 0x001fUL : 0x000000ffUL;
      _alphaMask = 0;
    } else {
      uint32_t maskOffset = 14 + _dibSize;
      if (_dibSize >= 52) maskOffset = 14 + 40;
      else extraMasks = _compression == Compression::AlphaBitfields ? 16 : 12;
      if (_dibSize >= 52) {
        _redMask = readLe32(dib + 40);
        _greenMask = readLe32(dib + 44);
        _blueMask = readLe32(dib + 48);
        _alphaMask = _dibSize >= 56 ? readLe32(dib + 52) : 0;
        if (_compression == Compression::AlphaBitfields && _dibSize < 56) {
          _error = F("Missing BMP alpha mask");
          return false;
        }
      } else {
        uint8_t masks[16];
        if (!_file.seek(maskOffset) || _file.read(masks, extraMasks) != extraMasks) {
          _error = F("Missing BMP bit masks");
          return false;
        }
        _redMask = readLe32(masks);
        _greenMask = readLe32(masks + 4);
        _blueMask = readLe32(masks + 8);
        _alphaMask = extraMasks == 16 ? readLe32(masks + 12) : 0;
      }
    }
    if (_redMask == 0 || _greenMask == 0 || _blueMask == 0) {
      _error = F("Invalid BMP channel masks");
      return false;
    }
    while ((_redMask & (1UL << _redShift)) == 0 && _redShift < 31) _redShift++;
    while ((_greenMask & (1UL << _greenShift)) == 0 && _greenShift < 31) _greenShift++;
    while ((_blueMask & (1UL << _blueShift)) == 0 && _blueShift < 31) _blueShift++;
    while (_alphaMask && (_alphaMask & (1UL << _alphaShift)) == 0 && _alphaShift < 31) _alphaShift++;
    _hasAlpha = _alphaMask != 0 && (_bitsPerPixel == 32);
  }

  uint32_t paletteBytes = _bitsPerPixel <= 8 ? 1UL << _bitsPerPixel : 0;
  _paletteCount = 0;
  if (paletteBytes) {
    if (colorsUsed != 0) paletteBytes = colorsUsed;
    if (paletteBytes > MaxPaletteEntries) {
      _error = F("BMP palette too large");
      return false;
    }
    uint32_t paletteOffset = 14 + _dibSize + extraMasks;
    if (!_file.seek(paletteOffset)) return false;
    for (uint32_t i = 0; i < paletteBytes; i++) {
      uint8_t entry[4];
      if (_file.read(entry, sizeof(entry)) != sizeof(entry)) {
        _error = F("Truncated BMP palette");
        return false;
      }
      _palette[i] = { entry[2], entry[1], entry[0] };
    }
    _paletteCount = (uint16_t)paletteBytes;
  }

  if (_pixelOffset < 14 + _dibSize + extraMasks + paletteBytes ||
      _pixelOffset >= _file.size()) {
    _error = F("Invalid BMP pixel offset");
    return false;
  }
  if (_rle) {
    _rowStride = 0;
    _rleFileRow = 0;
    _rleX = 0;
    _rlePendingBlankRows = 0;
    _rleDone = false;
  } else {
    uint64_t stride = (((uint64_t)_sourceWidth * _bitsPerPixel + 31) / 32) * 4;
    if (stride == 0 || stride > 0xffffffffULL) {
      _error = F("BMP row too large");
      return false;
    }
    _rowStride = (uint32_t)stride;
  }
  return true;
}

bool ImageViewerScreen::readRasterHeader() {
  uint8_t signature[8] = {};
  if (!_file.seek(0) || _file.read(signature, sizeof(signature)) != sizeof(signature)) {
    _error = F("Unable to read image header");
    return false;
  }
  if (signature[0] == 'B' && signature[1] == 'M') {
    _format = ImageFormat::Bmp;
    return readHeader();
  }
  if (signature[0] == 0x89 && signature[1] == 'P' && signature[2] == 'N' && signature[3] == 'G' &&
      signature[4] == 0x0d && signature[5] == 0x0a && signature[6] == 0x1a && signature[7] == 0x0a) {
    _format = ImageFormat::Png;
    _decoderOwner = this;
    int pngResult = _png.open(_path.c_str(), pngOpen, pngClose, pngRead, pngSeek, pngDraw);
    if (pngResult != PNG_SUCCESS) {
      _error = String(F("PNG header error ")) + pngResult;
      return false;
    }
    if (_png.isInterlaced()) {
      _error = F("Interlaced PNG unsupported");
      _png.close();
      return false;
    }
    _sourceWidth = (uint32_t)_png.getWidth();
    _sourceHeight = (uint32_t)_png.getHeight();
    if (_sourceWidth == 0 || _sourceHeight == 0) {
      _error = F("PNG has invalid dimensions");
      return false;
    }
    return true;
  }
  if (signature[0] == 0xff && signature[1] == 0xd8) {
    _format = ImageFormat::Jpeg;
    _decoderOwner = this;
    int jpegResult = _file.seek(0) ? _jpeg.open(_file, jpegDraw) : 0;
    if (jpegResult != 1) {
      _error = String(F("JPEG header error ")) + _jpeg.getLastError();
      return false;
    }
    if (_jpeg.getJPEGType() != JPEG_MODE_BASELINE) {
      _error = F("Progressive JPEG unsupported");
      _jpeg.close();
      return false;
    }
    _sourceWidth = (uint32_t)_jpeg.getWidth();
    _sourceHeight = (uint32_t)_jpeg.getHeight();
    if (_sourceWidth == 0 || _sourceHeight == 0) {
      _error = F("JPEG has invalid dimensions");
      return false;
    }
    return true;
  }
  _error = F("Unsupported image format");
  return false;
}

String ImageViewerScreen::cachePathForSource() const {
  int slash = _path.lastIndexOf('/');
  String directory = slash <= 0 ? F("/") : _path.substring(0, slash);
  String name = slash < 0 ? _path : _path.substring(slash + 1);
  return directory + (directory == F("/") ? F("") : F("/")) + F(".cache/") + name + F(".em16");
}

bool ImageViewerScreen::readCacheHeader(fs::File &file) const {
  if (!file || file.size() != CacheHeaderSize + CachePixelBytes) return false;
  uint8_t header[CacheHeaderSize];
  if (file.read(header, sizeof(header)) != sizeof(header)) return false;
  if (header[0] != 'E' || header[1] != 'M' || header[2] != '1' || header[3] != '6' ||
      header[4] != 1 || header[5] != 0 || readLe16(header + 8) != CacheWidth ||
      readLe16(header + 10) != CacheHeight || readLe32(header + 12) != _sourceFileSize ||
      readLe32(header + 16) != _sourceWriteTime || readLe32(header + 20) != CachePixelBytes) {
    return false;
  }
  return true;
}

bool ImageViewerScreen::loadCache() {
  if (!cacheNeedsRotation() && (_display.width() != CacheWidth || _display.height() != CacheHeight)) return false;
  if (!SD.exists(_cachePath)) return false;
  fs::File cache = SD.open(_cachePath.c_str(), FILE_READ);
  if (!cache || !readCacheHeader(cache)) {
    if (cache) cache.close();
    return false;
  }
  _frameBuffer = (uint16_t *)ps_malloc(CachePixelBytes);
  bool readOk = _frameBuffer != nullptr;
  uint32_t offset = 0;
  while (readOk && offset < CachePixelBytes) {
    uint32_t remaining = CachePixelBytes - offset;
    uint16_t amount = remaining > CacheChunkBytes ? CacheChunkBytes : (uint16_t)remaining;
    if (cache.read(_cacheChunk, amount) != amount) {
      readOk = false;
      break;
    }
    if (!cacheNeedsRotation()) {
      memcpy((uint8_t *)_frameBuffer + offset, _cacheChunk, amount);
    } else {
      uint32_t firstPixel = offset / 2;
      uint32_t pixelCount = amount / 2;
      for (uint32_t i = 0; i < pixelCount; i++) {
        uint32_t cachePixel = firstPixel + i;
        uint16_t color;
        memcpy(&color, _cacheChunk + i * 2, sizeof(color));
        uint32_t cacheX = cachePixel % CacheWidth;
        uint32_t cacheY = cachePixel / CacheWidth;
        uint32_t screenX = cacheY;
        uint32_t screenY = CacheWidth - 1 - cacheX;
        _frameBuffer[screenY * _display.width() + screenX] = color;
      }
    }
    offset += amount;
  }
  if (!readOk) {
    cache.close();
    releaseBuffers();
    return false;
  }
  cache.close();
  return true;
}

bool ImageViewerScreen::writeCacheHeader(fs::File &file) const {
  uint8_t header[CacheHeaderSize] = {};
  header[0] = 'E'; header[1] = 'M'; header[2] = '1'; header[3] = '6';
  header[4] = 1; // cache format version
  header[5] = 0; // RGB565 little-endian
  writeLe16(header + 8, CacheWidth);
  writeLe16(header + 10, CacheHeight);
  writeLe32(header + 12, _sourceFileSize);
  writeLe32(header + 16, _sourceWriteTime);
  writeLe32(header + 20, CachePixelBytes);
  return file.write(header, sizeof(header)) == sizeof(header);
}

void ImageViewerScreen::startCacheWrite() {
  if (_cacheWriting || !_frameBuffer || _cachePath.length() == 0) return;
  if (!cacheNeedsRotation() && (_display.width() != CacheWidth || _display.height() != CacheHeight)) {
    Serial.print(F("Image cache: skipped, display is "));
    Serial.print(_display.width());
    Serial.print('x');
    Serial.println(_display.height());
    return;
  }
  int slash = _cachePath.lastIndexOf('/');
  String directory = slash <= 0 ? F("/") : _cachePath.substring(0, slash);
  Serial.print(F("Image cache: preparing "));
  Serial.println(_cachePath);
  if (!SD.exists(directory) && !SD.mkdir(directory)) {
    Serial.print(F("Image cache: cannot create "));
    Serial.println(directory);
    return;
  }
  SD.remove(_cacheTempPath);
  _cacheFile = SD.open(_cacheTempPath.c_str(), FILE_WRITE);
  if (!_cacheFile || !writeCacheHeader(_cacheFile)) {
    Serial.print(F("Image cache: cannot start "));
    Serial.println(_cacheTempPath);
    if (_cacheFile) _cacheFile.close();
    SD.remove(_cacheTempPath);
    return;
  }
  _cacheOffset = 0;
  _cacheWriting = true;
  Serial.print(F("Image cache: writing "));
  Serial.println(_cacheTempPath);
}

void ImageViewerScreen::writeCacheChunk() {
  if (!_cacheWriting || !_cacheFile || !_frameBuffer) return;
  uint32_t remaining = CachePixelBytes - _cacheOffset;
  uint16_t amount = remaining > CacheChunkBytes ? CacheChunkBytes : (uint16_t)remaining;
  // Copy through internal RAM; SPI DMA cannot reliably source arbitrary PSRAM buffers.
  if (!cacheNeedsRotation()) {
    memcpy(_cacheChunk, (uint8_t *)_frameBuffer + _cacheOffset, amount);
  } else {
    uint32_t firstPixel = _cacheOffset / 2;
    uint32_t pixelCount = amount / 2;
    for (uint32_t i = 0; i < pixelCount; i++) {
      uint32_t cachePixel = firstPixel + i;
      uint32_t cacheX = cachePixel % CacheWidth;
      uint32_t cacheY = cachePixel / CacheWidth;
      uint32_t screenX = cacheY;
      uint32_t screenY = CacheWidth - 1 - cacheX;
      uint16_t color = _frameBuffer[screenY * _display.width() + screenX];
      memcpy(_cacheChunk + i * 2, &color, sizeof(color));
    }
  }
  size_t written = _cacheFile.write(_cacheChunk, amount);
  if (written != amount) {
    Serial.print(F("Image cache: write failed "));
    Serial.println(_cacheTempPath);
    finishCacheWrite(false);
    return;
  }
  _cacheOffset += amount;
  if (_cacheOffset == CachePixelBytes) finishCacheWrite(true);
}

bool ImageViewerScreen::cacheNeedsRotation() const {
  return _display.width() == CacheHeight && _display.height() == CacheWidth;
}

void ImageViewerScreen::finishCacheWrite(bool success) {
  if (_cacheFile) _cacheFile.close();
  bool committed = false;
  if (success) {
    bool removedOld = !_cachePath.length() || !SD.exists(_cachePath) || SD.remove(_cachePath);
    committed = removedOld && SD.rename(_cacheTempPath, _cachePath);
  }
  if (!committed) {
    SD.remove(_cacheTempPath);
    if (success) {
      Serial.print(F("Image cache: commit failed "));
      Serial.println(_cachePath);
    }
  }
  _cacheWriting = false;
  _cacheOffset = 0;
  if (committed) {
    Serial.print(F("Image cache ready: "));
    Serial.println(_cachePath);
  }
  releaseBuffers();
}

bool ImageViewerScreen::prepareRender() {
  if (!_display.isReady() || _sourceWidth == 0 || _sourceHeight == 0) {
    _error = F("Invalid image size");
    return false;
  }
  uint32_t screenWidth = _display.width();
  uint32_t screenHeight = _display.height();
  if ((uint64_t)screenWidth * _sourceHeight <= (uint64_t)screenHeight * _sourceWidth) {
    _drawWidth = screenWidth;
    _drawHeight = (uint16_t)(((uint64_t)screenWidth * _sourceHeight) / _sourceWidth);
  } else {
    _drawHeight = screenHeight;
    _drawWidth = (uint16_t)(((uint64_t)screenHeight * _sourceWidth) / _sourceHeight);
  }
  if (_drawWidth == 0) _drawWidth = 1;
  if (_drawHeight == 0) _drawHeight = 1;
  _left = (int16_t)((screenWidth - _drawWidth) / 2);
  _top = (int16_t)((screenHeight - _drawHeight) / 2);
  _nextDrawY = _drawHeight - 1;
  _lastSourceY = InvalidSourceRow;
  _lastProgress = 0;

  size_t sourceBytes = (size_t)_sourceWidth * 4;
  if (sourceBytes / 4 != _sourceWidth) {
    _error = F("Image dimensions unsupported");
    return false;
  }
  if (!psramFound()) {
    _error = F("Image viewer requires PSRAM");
    return false;
  }
  _frameBuffer = (uint16_t *)ps_malloc((size_t)screenWidth * screenHeight * sizeof(uint16_t));
  _xCoordinates = (uint32_t *)malloc((size_t)_drawWidth * sizeof(uint32_t));
  if (_format == ImageFormat::Bmp) {
    _row0 = (uint8_t *)malloc(sourceBytes);
    _row1 = (uint8_t *)malloc(sourceBytes);
    if (!_rle) _rawRow = (uint8_t *)malloc(_rowStride);
  }
  if (!_frameBuffer || !_xCoordinates ||
      (_format == ImageFormat::Bmp && (!_row0 || !_row1 || (!_rle && !_rawRow)))) {
    _error = F("Image needs too much memory");
    releaseBuffers();
    return false;
  }
  memset(_frameBuffer, 0, (size_t)screenWidth * screenHeight * sizeof(uint16_t));
  for (uint32_t dx = 0; dx < _drawWidth; dx++) {
    _xCoordinates[dx] = _drawWidth > 1
      ? (uint32_t)(((uint64_t)dx * (_sourceWidth - 1) << 16) / (_drawWidth - 1)) : 0;
  }
  return true;
}

bool ImageViewerScreen::decodeRasterImage() {
  int result = 0;
  if (_format == ImageFormat::Png) {
    result = _png.decode(this, PNG_CHECK_CRC);
    int pngError = _png.getLastError();
    _png.close();
    _decoderOwner = nullptr;
    if (result != PNG_SUCCESS) {
      _error = String(F("PNG decode error ")) + result + F("/code ") + pngError;
      return false;
    }
  } else if (_format == ImageFormat::Jpeg) {
    result = _jpeg.decode(0, 0, 0);
    int jpegError = _jpeg.getLastError();
    _jpeg.close();
    _decoderOwner = nullptr;
    if (result != 1) {
      _error = String(F("JPEG decode error ")) + jpegError;
      return false;
    }
  }
  _lastProgress = 100;
  drawLoading(100);
  return true;
}

void ImageViewerScreen::storeRasterPixel(uint32_t sourceX, uint32_t sourceY, uint16_t color) {
  if (!_frameBuffer || sourceX >= _sourceWidth || sourceY >= _sourceHeight) return;
  uint32_t dx0 = (uint32_t)(((uint64_t)sourceX * _drawWidth) / _sourceWidth);
  uint32_t dx1 = (uint32_t)((((uint64_t)(sourceX + 1) * _drawWidth) + _sourceWidth - 1) / _sourceWidth);
  uint32_t dy0 = (uint32_t)(((uint64_t)sourceY * _drawHeight) / _sourceHeight);
  uint32_t dy1 = (uint32_t)((((uint64_t)(sourceY + 1) * _drawHeight) + _sourceHeight - 1) / _sourceHeight);
  if (dx1 <= dx0) dx1 = dx0 + 1;
  if (dy1 <= dy0) dy1 = dy0 + 1;
  if (dx1 > _drawWidth) dx1 = _drawWidth;
  if (dy1 > _drawHeight) dy1 = _drawHeight;
  for (uint32_t dy = dy0; dy < dy1; dy++) {
    uint16_t *row = _frameBuffer + (size_t)(_top + dy) * _display.width() + _left;
    for (uint32_t dx = dx0; dx < dx1; dx++) row[dx] = color;
  }
}

void *ImageViewerScreen::pngOpen(const char *name, int32_t *size) {
  if (!_decoderOwner) return nullptr;
  _decoderOwner->_decoderFile = SD.open(name, FILE_READ);
  if (!_decoderOwner->_decoderFile) return nullptr;
  *size = (int32_t)_decoderOwner->_decoderFile.size();
  return &_decoderOwner->_decoderFile;
}

void ImageViewerScreen::pngClose(void *handle) {
  File *file = static_cast<File *>(handle);
  if (file) file->close();
}

int32_t ImageViewerScreen::pngRead(PNGFILE *file, uint8_t *buffer, int32_t length) {
  if (!file || !file->fHandle) return 0;
  File *source = static_cast<File *>(file->fHandle);
  return source->read(buffer, length);
}

int32_t ImageViewerScreen::pngSeek(PNGFILE *file, int32_t position) {
  if (!file || !file->fHandle || position < 0) return 0;
  File *source = static_cast<File *>(file->fHandle);
  return source->seek(position) ? position : 0;
}

int ImageViewerScreen::pngDraw(PNGDRAW *draw) {
  if (!_decoderOwner || !draw) return 0;
  ImageViewerScreen &viewer = *_decoderOwner;
  uint16_t *pixels = (uint16_t *)malloc((size_t)draw->iWidth * sizeof(uint16_t));
  if (!pixels) return 0;
  viewer._png.getLineAsRGB565(draw, pixels, PNG_RGB565_LITTLE_ENDIAN, 0);
  for (int x = 0; x < draw->iWidth; x++) viewer.storeRasterPixel((uint32_t)x, (uint32_t)draw->y, pixels[x]);
  free(pixels);
  viewer.updateDecodeProgress((uint8_t)(((uint32_t)(draw->y + 1) * 100) / viewer._sourceHeight));
  return 1;
}

int ImageViewerScreen::jpegDraw(JPEGDRAW *draw) {
  if (!_decoderOwner || !draw || !draw->pPixels) return 0;
  ImageViewerScreen &viewer = *_decoderOwner;
  for (int y = 0; y < draw->iHeight; y++) {
    for (int x = 0; x < draw->iWidthUsed; x++) {
      viewer.storeRasterPixel((uint32_t)(draw->x + x), (uint32_t)(draw->y + y), draw->pPixels[y * draw->iWidth + x]);
    }
  }
  viewer.updateDecodeProgress((uint8_t)(((uint32_t)(draw->y + draw->iHeight) * 100) / viewer._sourceHeight));
  return 1;
}

bool ImageViewerScreen::renderRows(uint8_t maxRows) {
  if (!_rendering && _nextDrawY < 0) return true;
  uint8_t done = 0;
  while (done < maxRows && _nextDrawY >= 0) {
    uint32_t dy = (uint32_t)_nextDrawY;
    uint32_t y16 = _drawHeight > 1
      ? (uint32_t)(((uint64_t)dy * (_sourceHeight - 1) << 16) / (_drawHeight - 1)) : 0;
    uint32_t y0 = y16 >> 16;
    if (_rle) {
      if (_lastSourceY == InvalidSourceRow || y0 != _lastSourceY) {
        if (!readSourceRow(y0, _row0)) return false;
        _lastSourceY = y0;
      }
    } else if (_lastSourceY == InvalidSourceRow || y0 != _lastSourceY) {
      if (!readSourceRow(y0, _row0)) return false;
      _lastSourceY = y0;
    }
    uint16_t *destination = _frameBuffer +
      (size_t)(_top + _nextDrawY) * _display.width() + _left;
    for (uint32_t dx = 0; dx < _drawWidth; dx++) {
      destination[dx] = blendPixel(_row0, _row1, _xCoordinates[dx], y16 & 0xffffUL);
    }
    _nextDrawY--;
    done++;
    uint8_t percent = (uint8_t)(((uint32_t)(_drawHeight - 1 - _nextDrawY) * 100) / _drawHeight);
    updateDecodeProgress(percent);
  }
  if (_nextDrawY < 0) {
    _rendering = false;
    _display.tft().pushImage(0, 0, _display.width(), _display.height(), _frameBuffer);
    startCacheWrite();
    if (!_cacheWriting) releaseBuffers();
    drawCloseIcon();
  }
  return true;
}

bool ImageViewerScreen::readSourceRow(uint32_t sourceY, uint8_t *target) {
  if (_rle) {
    // RLE streams are bottom-up. Skip source rows when aspect-fit output
    // samples fewer rows than the source contains.
    while (_rleFileRow < _sourceHeight && _sourceHeight - 1 - _rleFileRow > sourceY) {
      if (!readRleRow(_row0)) return false;
      _rleFileRow++;
    }
    if (_sourceHeight - 1 - _rleFileRow != sourceY) return false;
    if (_rleFileRow == 0) {
      if (!readRleRow(target)) return false;
      _rleFileRow++;
      memcpy(_row1, target, (size_t)_sourceWidth * 4);
    } else {
      memcpy(_row1, target, (size_t)_sourceWidth * 4);
      if (!readRleRow(target)) return false;
      _rleFileRow++;
    }
    return true;
  }
  if (!readUncompressedRow(sourceY, target)) return false;
  uint32_t nextY = sourceY < _sourceHeight - 1 ? sourceY + 1 : sourceY;
  if (nextY == sourceY) {
    memcpy(_row1, target, (size_t)_sourceWidth * 4);
    return true;
  }
  return readUncompressedRow(nextY, _row1);
}

bool ImageViewerScreen::readUncompressedRow(uint32_t sourceY, uint8_t *target) {
  uint32_t fileRow = _topDown ? sourceY : _sourceHeight - 1 - sourceY;
  uint64_t offset = (uint64_t)_pixelOffset + (uint64_t)fileRow * _rowStride;
  if (offset > 0xffffffffULL || !_file.seek((uint32_t)offset) ||
      _file.read(_rawRow, _rowStride) != _rowStride) return false;
  for (uint32_t x = 0; x < _sourceWidth; x++) {
    uint32_t value = 0;
    uint8_t index = 0;
    if (_bitsPerPixel == 1) index = (_rawRow[x >> 3] >> (7 - (x & 7))) & 1;
    else if (_bitsPerPixel == 4) index = (x & 1) ? (_rawRow[x >> 1] & 0x0f) : (_rawRow[x >> 1] >> 4);
    else if (_bitsPerPixel == 8) index = _rawRow[x];
    else if (_bitsPerPixel == 16) value = readLe16(_rawRow + x * 2);
    else if (_bitsPerPixel == 24) {
      const uint8_t *p = _rawRow + x * 3;
      setPixel(target, x, p[2], p[1], p[0]);
      continue;
    } else value = readLe32(_rawRow + x * 4);
    if (_bitsPerPixel <= 8) {
      if (index >= _paletteCount) return false;
      setPixel(target, x, _palette[index].r, _palette[index].g, _palette[index].b);
    } else {
      uint8_t r = extractChannel(value, _redMask, _redShift);
      uint8_t g = extractChannel(value, _greenMask, _greenShift);
      uint8_t b = extractChannel(value, _blueMask, _blueShift);
      uint8_t a = _hasAlpha ? extractChannel(value, _alphaMask, _alphaShift) : 255;
      if (_bitsPerPixel == 32 && _compression == Compression::Rgb) {
        uint8_t storedAlpha = (uint8_t)(value >> 24);
        a = storedAlpha == 0 ? 255 : storedAlpha;
      }
      setPixel(target, x, r, g, b, a);
    }
  }
  return true;
}

bool ImageViewerScreen::readRleByte(uint8_t &value) {
  return _file.read(&value, 1) == 1;
}

bool ImageViewerScreen::readRleRow(uint8_t *target) {
  memset(target, 0, (size_t)_sourceWidth * 4);
  if (_rlePendingBlankRows) {
    _rlePendingBlankRows--;
    return true;
  }
  if (_rleDone) return false;
  _rleX = 0;
  while (true) {
    uint8_t count, command;
    if (!readRleByte(count) || !readRleByte(command)) return false;
    if (count) {
      for (uint8_t i = 0; i < count; i++) {
        uint8_t index = _compression == Compression::Rle8
          ? command : ((i & 1) ? (command & 0x0f) : (command >> 4));
        if (_rleX >= _sourceWidth || index >= _paletteCount) return false;
        setPixel(target, _rleX++, _palette[index].r, _palette[index].g, _palette[index].b);
      }
      continue;
    }
    if (command == 0) return true;
    if (command == 1) {
      _rleDone = true;
      return true;
    }
    if (command == 2) {
      uint8_t dx, dy;
      if (!readRleByte(dx) || !readRleByte(dy) || _rleX + dx > _sourceWidth) return false;
      _rleX += dx;
      _rlePendingBlankRows = dy;
      return true;
    }
    uint8_t n = command;
    uint8_t bytes = _compression == Compression::Rle8 ? n : (uint8_t)((n + 1) / 2);
    uint8_t packed = 0;
    for (uint8_t i = 0; i < n; i++) {
      if (i % (_compression == Compression::Rle8 ? 1 : 2) == 0 && !readRleByte(packed)) return false;
      uint8_t index;
      if (_compression == Compression::Rle8) index = packed;
      else index = (i & 1) ? (packed & 0x0f) : (packed >> 4);
      if (_rleX >= _sourceWidth || index >= _paletteCount) return false;
      setPixel(target, _rleX++, _palette[index].r, _palette[index].g, _palette[index].b);
    }
    if ((bytes & 1) && !readRleByte(command)) return false;
  }
}

void ImageViewerScreen::setPixel(uint8_t *row, uint32_t x, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (x >= _sourceWidth) return;
  if (a != 255) {
    r = (uint8_t)(((uint16_t)r * a) / 255);
    g = (uint8_t)(((uint16_t)g * a) / 255);
    b = (uint8_t)(((uint16_t)b * a) / 255);
  }
  uint8_t *p = row + x * 4;
  p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
}

uint8_t ImageViewerScreen::extractChannel(uint32_t value, uint32_t mask, uint8_t shift) const {
  if (!mask) return 0;
  uint32_t raw = (value & mask) >> shift;
  uint32_t max = mask >> shift;
  return (uint8_t)((raw * 255UL + max / 2) / max);
}

uint16_t ImageViewerScreen::blendPixel(const uint8_t *row0,
                                       const uint8_t *row1,
                                       uint32_t sourceX16,
                                       uint32_t sourceY16) const {
  uint32_t x0 = sourceX16 >> 16;
  uint32_t x1 = x0 < _sourceWidth - 1 ? x0 + 1 : x0;
  uint32_t fx = sourceX16 & 0xffffUL;
  uint32_t fy = sourceY16;
  const uint8_t *a = row0 + x0 * 4;
  const uint8_t *b = row0 + x1 * 4;
  const uint8_t *c = row1 + x0 * 4;
  const uint8_t *d = row1 + x1 * 4;
  uint32_t r0 = (a[0] * (65536UL - fx) + b[0] * fx) >> 16;
  uint32_t g0 = (a[1] * (65536UL - fx) + b[1] * fx) >> 16;
  uint32_t b0 = (a[2] * (65536UL - fx) + b[2] * fx) >> 16;
  uint32_t r1 = (c[0] * (65536UL - fx) + d[0] * fx) >> 16;
  uint32_t g1 = (c[1] * (65536UL - fx) + d[1] * fx) >> 16;
  uint32_t b1 = (c[2] * (65536UL - fx) + d[2] * fx) >> 16;
  uint8_t r = (uint8_t)((r0 * (65536UL - fy) + r1 * fy) >> 16);
  uint8_t g = (uint8_t)((g0 * (65536UL - fy) + g1 * fy) >> 16);
  uint8_t bl = (uint8_t)((b0 * (65536UL - fy) + b1 * fy) >> 16);
  return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (bl >> 3));
}

void ImageViewerScreen::releaseBuffers() {
  if (_frameBuffer) free(_frameBuffer);
  if (_xCoordinates) free(_xCoordinates);
  if (_rawRow) free(_rawRow);
  if (_row0) free(_row0);
  if (_row1) free(_row1);
  _frameBuffer = nullptr;
  _xCoordinates = nullptr;
  _rawRow = nullptr;
  _row0 = nullptr;
  _row1 = nullptr;
}

uint16_t ImageViewerScreen::readLe16(const uint8_t *buffer) const {
  return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

uint32_t ImageViewerScreen::readLe32(const uint8_t *buffer) const {
  return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
         ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
}

int32_t ImageViewerScreen::readLe32Signed(const uint8_t *buffer) const {
  return (int32_t)readLe32(buffer);
}

void ImageViewerScreen::writeLe16(uint8_t *buffer, uint16_t value) const {
  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
}

void ImageViewerScreen::writeLe32(uint8_t *buffer, uint32_t value) const {
  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
  buffer[2] = (uint8_t)(value >> 16);
  buffer[3] = (uint8_t)(value >> 24);
}
