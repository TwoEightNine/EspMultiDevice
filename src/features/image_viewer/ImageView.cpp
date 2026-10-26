#include "ImageView.h"

#include "../../media/FileFormats.h"

#include "../../ui/UiKit.h"

#include <SD.h>
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include "../../core/SpiBusLock.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {
const uint32_t InvalidSourceRow = 0xffffffffUL;
const uint32_t MaxSupportedDibSize = 124;
// A Lua app worker may hold the shared SD/touch bus while the viewer decodes
// for it, so every source transaction takes the lock like MediaCache does.
const uint32_t SourceLockTimeoutMs = 250;

int lockedRead(StorageFile &file, uint8_t *buffer, size_t length) {
  if (!SpiBusLock::lock(pdMS_TO_TICKS(SourceLockTimeoutMs))) return -1;
  const int amount = file.read(buffer, length);
  SpiBusLock::unlock();
  return amount;
}

bool lockedSeek(StorageFile &file, uint32_t position) {
  if (!SpiBusLock::lock(pdMS_TO_TICKS(SourceLockTimeoutMs))) return false;
  const bool moved = file.seek(position);
  SpiBusLock::unlock();
  return moved;
}
// Version 1 used the same display-ready pixels, but its large-animation path
// never enabled the second-core ring loader. Bump the cache version so old
// files are rebuilt with the corrected playback contract.
}

ImageView *ImageView::_decoderOwner = nullptr;

ImageView::ImageView(DisplayController &display,
                     SdCardController &sdCard,
                     SecretStorage &secretStorage,
                     MediaCache &mediaCache)
  : _display(display),
    _sdCard(sdCard),
    _secretStorage(secretStorage),
    _mediaCache(mediaCache),
    _secretAccess(false),
    _webpSource(nullptr),
    _webpSourceSize(0),
    _webpRgba(nullptr),
    _animationSource(false),
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
    _sourceCropX(0),
    _sourceCropY(0),
    _sourceCropWidth(0),
    _sourceCropHeight(0),
    _viewMode(ViewMode::Informative),
    _cropZoom(false),
    _imageReady(false),
    _writeCacheAfterRender(true),
    _nextDrawY(-1),
    _lastSourceY(InvalidSourceRow),
    _frameBuffer(nullptr),
    _xCoordinates(nullptr),
    _rawRow(nullptr),
    _row0(nullptr),
    _row1(nullptr),
    _rendering(false),
    _cacheWriting(false),
    _streamPresentedFrames(0),
    _streamStatsAtMs(0),
    _openedAtMs(0),
    _streamPresentY(0),
    _streamPresentActive(false),
    _lastProgress(0),
    _rleFileRow(0),
    _rleX(0),
    _rlePendingBlankRows(0),
    _rleDone(false),
    _stream(_secretStorage),
    _streamSlot(AnimationStreamWorker::NoSlot),
    _streamPixels(nullptr),
    _streamLoadingAtMs(0),
    _streamLoadingDrawnMs(0),
    _streamTransferActive(false),
    _streamDmaFallbackLogged(false) {
}

void ImageView::setChromePainter(ChromePainter painter, void *context) {
  _chromePainter = painter;
  _chromeContext = context;
}

StorageFile ImageView::openSource(const char *path) {
  if (!SpiBusLock::lock(pdMS_TO_TICKS(SourceLockTimeoutMs))) return StorageFile();
  StorageFile file = _secretStorage.open(path, FILE_READ, _secretAccess);
  SpiBusLock::unlock();
  return file;
}

void ImageView::open(const String &path, bool secretAccess, bool chromeVisible) {
  reset();
  _openedAtMs = millis();
  _secretAccess = secretAccess;
  _viewMode = chromeVisible ? ViewMode::Informative : ViewMode::FullScreen;
  if (path.length() == 0) {
    _error = F("No image selected");
    drawError();
    return;
  }
  _path = path;
  // Put a real frame on the panel before touching SD or allocating decode
  // buffers.  Animated formats continue their build incrementally in tick(),
  // and this prevents a blank/previous Files screen during header/cache work.
  draw();
  yield();
  if (!_sdCard.isMounted()) {
    _error = F("SD card unavailable");
    drawError();
    return;
  }
  _file = openSource(_path.c_str());
  if (!_file) {
    _error = F("Unable to open source image");
    drawError();
    return;
  }
  SourceFingerprint fingerprint{};
  if (_mediaCache.captureFingerprint(_file, fingerprint) != MediaCache::Result::Complete) {
    _error = F("Unable to inspect source image");
    drawError();
    return;
  }
  _sourceFileSize = fingerprint.size;
  _sourceWriteTime = fingerprint.modified;
  if (isGifSource()) {
    _format = ImageFormat::Gif;
    _animationSource = true;
    if (!readGifStreamHeader() || !startAnimationStream(AnimationStreamWorker::Source::Gif)) {
      if (_error.length() == 0) _error = F("Could not start animation");
      stopAnimationStream();
      drawError();
    }
    return;
  }
  if (isWebpSource()) {
    _format = ImageFormat::Webp;
    // Animated WebP streams from its source; only stills are cached.
    if (sniffAnimatedWebp()) {
      _animationSource = true;
      if (!startAnimationStream(AnimationStreamWorker::Source::Webp)) {
        if (_error.length() == 0) _error = F("Could not start animation");
        stopAnimationStream();
        drawError();
      }
      return;
    }
    if (_error.length() == 0 && !inspectWebpSource() && _error.length() == 0) {
      _error = F("Invalid WebP image");
    }
    if (_error.length() != 0) {
      releaseWebpDecoder();
      drawError();
      return;
    }
    if (!prepareCachePaths(FileFormats::Format::Em16)) {
      _error = F("Invalid cache path");
      drawError();
      return;
    }
    if (!prepareCachePaths(FileFormats::Format::Em16Zoom)) {
      _error = F("Invalid zoom cache path");
      drawError();
      return;
    }
  } else {
    if (!prepareCachePaths(FileFormats::Format::Em16)) {
      _error = F("Invalid cache path");
      drawError();
      return;
    }
    if (!prepareCachePaths(FileFormats::Format::Em16Zoom)) {
      _error = F("Invalid zoom cache path");
      drawError();
      return;
    }
  }
  if (loadCache(false)) {
    Serial.print(F("Image cache hit: "));
    Serial.println(_cachePath);
    _imageReady = true;
    presentStaticFrame();
    releaseWebpDecoder();
    return;
  }
  if (!readRasterHeader() || !prepareRender()) {
    if (_error.length() == 0) _error = F("Unable to render image");
    releaseWebpDecoder();
    drawError();
    return;
  }
  drawLoading(0);
  if (_format != ImageFormat::Bmp && !decodeRasterImage()) {
    if (_error.length() == 0) _error = F("Could not decode image");
    releaseBuffers();
    releaseWebpDecoder();
    drawError();
    return;
  }
  _rendering = _format == ImageFormat::Bmp;
  if (!_rendering) {
    _imageReady = true;
    presentStaticFrame();
    startCacheWrite();
    releaseRenderScratch();
    releaseWebpDecoder();
    return;
  }
  if (!renderRows(16)) {
    _rendering = false;
    if (_error.length() == 0) _error = F("Could not read image data");
    releaseBuffers();
    drawError();
    return;
  }
}

void ImageView::close() {
  reset();
}

void ImageView::tick(uint32_t nowMs) {
  if (_stream.active()) {
    if (!tickAnimationStream(nowMs)) {
      if (_error.length() == 0) _error = F("Animation playback error");
      stopAnimationStream();
      drawError();
    }
    return;
  }
  if (_rendering && !renderRows(16)) {
    _rendering = false;
    if (_error.length() == 0) _error = F("Could not read image data");
    releaseBuffers();
    drawError();
  }
  if (_cacheWriting) writeCacheChunk();
}

void ImageView::reset() {
  // Stop any stream before freeing anything its frames might reference.
  stopAnimationStream();
  _rendering = false;
  _decoderOwner = nullptr;
  releaseWebpDecoder();
  if (_file) _file.close();
  if (_decoderFile) _decoderFile.close();
  if (_cacheWriter.active) _mediaCache.abort(_cacheWriter);
  _cacheWriting = false;
  _viewMode = ViewMode::Informative;
  _cropZoom = false;
  _imageReady = false;
  _writeCacheAfterRender = true;
  releaseBuffers();
  _path = "";
  _error = "";
  _failed = false;
  _cachePath = "";
  _cacheTempPath = "";
  _zoomCachePath = "";
  _zoomCacheTempPath = "";
  _sourceFileSize = 0;
  _sourceWriteTime = 0;
  _cacheOffset = 0;
  _animationSource = false;
  _streamPresentedFrames = 0;
  _streamStatsAtMs = 0;
  _openedAtMs = 0;
  _streamPresentY = 0;
  _streamPresentActive = false;
  _format = ImageFormat::Unknown;
  _pixelOffset = 0;
  _sourceWidth = 0;
  _sourceHeight = 0;
  _sourceCropX = 0;
  _sourceCropY = 0;
  _sourceCropWidth = 0;
  _sourceCropHeight = 0;
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

void ImageView::draw() {
  if (!_display.isReady()) return;
  _display.tft().fillScreen(TFT_BLACK);
  // Chrome is painted once per full-screen refresh. Incremental loader
  // updates only touch their bounded content area, so they cannot flash the
  // app bar while source rows are being decoded.
  drawChrome();
}

String ImageView::filename() const {
  int slash = _path.lastIndexOf('/');
  return slash >= 0 ? _path.substring(slash + 1) : _path;
}

bool ImageView::isStaticImage() const {
  return _sourceWidth > 0 && _sourceHeight > 0 && !_animationSource;
}

bool ImageView::zoomAvailable() const {
  if (!_imageReady || !isStaticImage() || _display.height() == 0 || _sourceHeight == 0) return false;
  const uint64_t imageRatio = (uint64_t)_sourceWidth * _display.height();
  const uint64_t screenRatio = (uint64_t)_sourceHeight * _display.width();
  const uint64_t difference = imageRatio > screenRatio ? imageRatio - screenRatio : screenRatio - imageRatio;
  return difference * 10ULL > screenRatio;
}

void ImageView::drawChrome() {
  if (!_display.isReady() || _viewMode == ViewMode::FullScreen || !_chromePainter) return;
  _chromePainter(_chromeContext);
}

void ImageView::redraw() {
  if (_failed) {
    drawError();
  } else if (isStaticImage() && _frameBuffer) {
    presentStaticFrame();
  } else if (_stream.active()) {
    draw();
    if (_streamPixels) presentStreamFrameNow();
    else _streamLoadingAtMs = 0;
  } else {
    draw();
  }
}

void ImageView::presentStaticFrame() {
  if (!_display.isReady() || !_frameBuffer) return;
  _display.tft().pushImage(0, 0, _display.width(), _display.height(), _frameBuffer);
  drawChrome();
}

bool ImageView::reloadStaticRender(bool cropZoom) {
  if (!isStaticImage()) return false;
  if (_cacheWriter.active) _mediaCache.abort(_cacheWriter);
  _cacheWriting = false;
  _cacheOffset = 0;
  _rendering = false;
  _nextDrawY = -1;
  releaseBuffers();
  releaseWebpDecoder();
  _cropZoom = cropZoom;
  _imageReady = false;
  _writeCacheAfterRender = true;
  _error = "";
  _failed = false;

  if (!readRasterHeader() || !prepareRender()) {
    if (_error.length() == 0) _error = F("Unable to render image");
    releaseWebpDecoder();
    drawError();
    return false;
  }
  draw();
  drawLoading(0);
  if (_format != ImageFormat::Bmp && !decodeRasterImage()) {
    if (_error.length() == 0) _error = F("Could not decode image");
    releaseBuffers();
    releaseWebpDecoder();
    drawError();
    return false;
  }
  _rendering = _format == ImageFormat::Bmp;
  if (_rendering) return true;

  _imageReady = true;
  presentStaticFrame();
  if (_writeCacheAfterRender) startCacheWrite();
  releaseRenderScratch();
  releaseWebpDecoder();
  return true;
}

void ImageView::setCropZoom(bool cropZoom) {
  if (!zoomAvailable() || cropZoom == _cropZoom) return;
  if (_cacheWriter.active) _mediaCache.abort(_cacheWriter);
  _cacheWriting = false;
  _cacheOffset = 0;
  releaseBuffers();
  releaseWebpDecoder();
  _cropZoom = cropZoom;
  _imageReady = false;
  _writeCacheAfterRender = true;
  if (loadCache(cropZoom)) {
    _imageReady = true;
    presentStaticFrame();
    return;
  }
  (void)reloadStaticRender(cropZoom);
}

void ImageView::setChromeVisible(bool visible) {
  const ViewMode mode = visible ? ViewMode::Informative : ViewMode::FullScreen;
  if (mode == _viewMode) return;
  _viewMode = mode;
  redraw();
}

void ImageView::drawError() {
  _failed = true;
  draw();
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  int16_t centerX = _display.width() / 2;
  int16_t centerY = _display.height() / 2;
  UiText::draw(tft, F("Image unavailable"), centerX, centerY - 40, TFT_WHITE, TFT_BLACK,
               UiText::Size::Title, MC_DATUM);
  UiText::draw(tft, _error, centerX, centerY - 12, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  String details = String(F("Format: ")) + formatName() + F("  Size: ") +
                   String(_sourceWidth) + F("x") + String(_sourceHeight);
  UiText::draw(tft, details, centerX, centerY + 12, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  String imageFilename = filename();
  if (imageFilename.length() > 28) imageFilename = String(F("...")) + imageFilename.substring(imageFilename.length() - 25);
  UiText::draw(tft, imageFilename, centerX, centerY + 36, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
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
}

const char *ImageView::formatName() const {
  switch (_format) {
    case ImageFormat::Bmp: return "BMP";
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Jpeg: return "JPEG";
    case ImageFormat::Gif: return "GIF";
    case ImageFormat::Webp: return "WebP";
    default: return "unknown";
  }
}

void ImageView::drawLoading(uint8_t percent) {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  int16_t centerX = _display.width() / 2;
  int16_t centerY = _display.height() / 2;
  tft.fillRect(centerX - 110, centerY - 26, 220, 52, TFT_BLACK);
  UiText::draw(tft, F("Loading image"), centerX, centerY - 11, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  UiKit::drawProgressBar(tft, centerX - 90, centerY + 2, 180, 12, percent, false,
                         TFT_BLUE, TFT_DARKGREY, TFT_BLACK);
}

void ImageView::updateDecodeProgress(uint8_t percent) {
  if (percent == 100 || percent >= (uint8_t)(_lastProgress + 5)) {
    _lastProgress = percent;
    drawLoading(percent);
  }
}

bool ImageView::readHeader() {
  uint8_t fileHeader[14];
  if (!lockedSeek(_file, 0) || lockedRead(_file, fileHeader, sizeof(fileHeader)) != sizeof(fileHeader) ||
      fileHeader[0] != 'B' || fileHeader[1] != 'M') {
    _error = F("Invalid BMP header");
    return false;
  }
  _pixelOffset = readLe32(fileHeader + 10);

  uint8_t dib[MaxSupportedDibSize];
  if (lockedRead(_file, dib, 4) != 4) {
    _error = F("Missing BMP DIB header");
    return false;
  }
  _dibSize = readLe32(dib);
  if (_dibSize < 40 || _dibSize > MaxSupportedDibSize ||
      lockedRead(_file, dib + 4, _dibSize - 4) != _dibSize - 4) {
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
        if (!lockedSeek(_file, maskOffset) || lockedRead(_file, masks, extraMasks) != extraMasks) {
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
    if (!lockedSeek(_file, paletteOffset)) return false;
    for (uint32_t i = 0; i < paletteBytes; i++) {
      uint8_t entry[4];
      if (lockedRead(_file, entry, sizeof(entry)) != sizeof(entry)) {
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

bool ImageView::readRasterHeader() {
  uint8_t signature[12] = {};
  if (!lockedSeek(_file, 0) || lockedRead(_file, signature, sizeof(signature)) != sizeof(signature)) {
    _error = F("Unable to read image header");
    return false;
  }
  FileFormats::Format sourceFormat = FileFormats::fromSignature(signature, sizeof(signature));
  if (sourceFormat == FileFormats::Format::Bmp) {
    _format = ImageFormat::Bmp;
    return readHeader();
  }
  if (sourceFormat == FileFormats::Format::Png) {
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
    const int pngWidth = _png.getWidth();
    const int pngHeight = _png.getHeight();
    const int bitsPerChannel = _png.getBpp();
    const int pixelType = _png.getPixelType();
    const int channels = pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED ? 1 :
                         pixelType == PNG_PIXEL_TRUECOLOR ? 3 :
                         pixelType == PNG_PIXEL_GRAY_ALPHA ? 2 :
                         pixelType == PNG_PIXEL_TRUECOLOR_ALPHA ? 4 : 0;
    const uint64_t scanlineBytes = channels > 0 && bitsPerChannel > 0
      ? ((uint64_t)pngWidth * (uint32_t)channels * (uint32_t)bitsPerChannel + 7) / 8 : 0;
    if (pngWidth <= 0 || pngHeight <= 0 || scanlineBytes == 0 || scanlineBytes > MaxPngScanlineBytes) {
      _error = F("PNG row exceeds decoder limit");
      _png.close();
      _decoderOwner = nullptr;
      return false;
    }
    _sourceWidth = (uint32_t)pngWidth;
    _sourceHeight = (uint32_t)pngHeight;
    return true;
  }
  if (sourceFormat == FileFormats::Format::Jpeg) {
    _format = ImageFormat::Jpeg;
    _decoderOwner = this;
    int jpegResult = lockedSeek(_file, 0) ? _jpeg.open(_path.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, jpegDraw) : 0;
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
  if (sourceFormat == FileFormats::Format::Webp) {
    _format = ImageFormat::Webp;
    if (_webpSource == nullptr && !inspectWebpSource()) return false;
    return true;
  }
  _error = F("Unsupported image format");
  return false;
}

bool ImageView::prepareCachePaths(FileFormats::Format format) {
  CachePaths paths;
  if (!CachePath::forSource(_path, format, paths)) return false;
  if (format == FileFormats::Format::Em16Zoom) {
    _zoomCachePath = paths.finalPath;
    _zoomCacheTempPath = paths.temporaryPath;
  } else {
    _cachePath = paths.finalPath;
    _cacheTempPath = paths.temporaryPath;
  }
  return true;
}

bool ImageView::readCacheHeader(StorageFile &file,
                                        uint32_t &sourceWidth,
                                        uint32_t &sourceHeight) const {
  uint32_t fileBytes = 0;
  if (!file || _mediaCache.fileSize(file, fileBytes) != MediaCache::Result::Complete ||
      fileBytes != CacheHeaderSize + CachePixelBytes) return false;
  uint8_t header[CacheHeaderSize];
  if (_mediaCache.read(file, header, sizeof(header)) != MediaCache::Result::Complete) return false;
  if (header[0] != 'E' || header[1] != 'M' || header[2] != '1' || header[3] != '6' ||
      header[4] != 2 || header[5] != 0 || readLe16(header + 8) != CacheWidth ||
      readLe16(header + 10) != CacheHeight ||
      !MediaCache::matches({readLe32(header + 12), readLe32(header + 16)},
                           {_sourceFileSize, _sourceWriteTime}) ||
      readLe32(header + 20) != CachePixelBytes || readLe32(header + 24) == 0 ||
      readLe32(header + 28) == 0) {
    return false;
  }
  sourceWidth = readLe32(header + 24);
  sourceHeight = readLe32(header + 28);
  return true;
}

bool ImageView::loadCache(bool cropZoom) {
  // Still formats intentionally probe their static cache before parsing the
  // source header, so their format is still Unknown here. Animations stream
  // and never have a cache.
  if (_animationSource) return false;
  if (!cacheNeedsRotation() && (_display.width() != CacheWidth || _display.height() != CacheHeight)) return false;
  CachePaths paths;
  const FileFormats::Format cacheFormat = cropZoom ? FileFormats::Format::Em16Zoom
                                                     : FileFormats::Format::Em16;
  if (!CachePath::forSource(_path, cacheFormat, paths)) return false;
  StorageFile cache;
  if (_mediaCache.openRead(paths, _secretAccess, cache) != MediaCache::Result::Complete) return false;
  uint32_t sourceWidth = 0;
  uint32_t sourceHeight = 0;
  if (!cache || !readCacheHeader(cache, sourceWidth, sourceHeight)) {
    _mediaCache.close(cache);
    return false;
  }
  _frameBuffer = (uint16_t *)ps_malloc(CachePixelBytes);
  bool readOk = _frameBuffer != nullptr;
  uint32_t offset = 0;
  while (readOk && offset < CachePixelBytes) {
    uint32_t remaining = CachePixelBytes - offset;
    uint16_t amount = remaining > CacheChunkBytes ? CacheChunkBytes : (uint16_t)remaining;
    if (_mediaCache.read(cache, _cacheChunk, amount) != MediaCache::Result::Complete) {
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
    _mediaCache.close(cache);
    releaseBuffers();
    return false;
  }
  _mediaCache.close(cache);
  _sourceWidth = sourceWidth;
  _sourceHeight = sourceHeight;
  return true;
}

void ImageView::prepareAnimationDrawRect() {
  uint32_t limitWidth = _display.width() < AnimationMaxWidth ? _display.width() : AnimationMaxWidth;
  uint32_t limitHeight = _display.height() < AnimationMaxHeight ? _display.height() : AnimationMaxHeight;
  if (_sourceWidth <= limitWidth && _sourceHeight <= limitHeight) {
    _drawWidth = (uint16_t)_sourceWidth;
    _drawHeight = (uint16_t)_sourceHeight;
  } else if ((uint64_t)limitWidth * _sourceHeight <= (uint64_t)limitHeight * _sourceWidth) {
    _drawWidth = (uint16_t)limitWidth;
    _drawHeight = (uint16_t)(((uint64_t)limitWidth * _sourceHeight) / _sourceWidth);
  } else {
    _drawHeight = (uint16_t)limitHeight;
    _drawWidth = (uint16_t)(((uint64_t)limitHeight * _sourceWidth) / _sourceHeight);
  }
}

bool ImageView::readGifStreamHeader() {
  uint8_t header[10] = {};
  if (!lockedSeek(_file, 0) || lockedRead(_file, header, sizeof(header)) != sizeof(header)) {
    _error = F("Unable to read GIF header");
    return false;
  }
  _sourceWidth = readLe16(header + 6);
  _sourceHeight = readLe16(header + 8);
  if (_sourceWidth == 0 || _sourceHeight == 0) {
    _error = F("GIF has invalid dimensions");
    return false;
  }
  return true;
}

bool ImageView::sniffAnimatedWebp() {
  // RIFF header, then an extended VP8X chunk whose flags carry the animation
  // bit and whose 24-bit fields hold the canvas size minus one.
  uint8_t header[30] = {};
  if (!lockedSeek(_file, 0) || lockedRead(_file, header, sizeof(header)) != sizeof(header)) {
    // Simple WebP files can be shorter than an extended header.
    if (_sourceFileSize >= sizeof(header)) _error = F("Unable to read WebP header");
    return false;
  }
  if (memcmp(header + 12, "VP8X", 4) != 0 || (header[20] & 0x02) == 0) return false;
  _sourceWidth = 1UL + header[24] + ((uint32_t)header[25] << 8) + ((uint32_t)header[26] << 16);
  _sourceHeight = 1UL + header[27] + ((uint32_t)header[28] << 8) + ((uint32_t)header[29] << 16);
  return true;
}

bool ImageView::startAnimationStream(AnimationStreamWorker::Source source) {
  if (!psramFound()) {
    _error = F("Animation viewer requires PSRAM");
    return false;
  }
  prepareAnimationDrawRect();
  if (_drawWidth == 0) _drawWidth = 1;
  if (_drawHeight == 0) _drawHeight = 1;
  _left = (int16_t)((_display.width() - _drawWidth) / 2);
  _top = (int16_t)((_display.height() - _drawHeight) / 2);
  _streamSlot = AnimationStreamWorker::NoSlot;
  _streamPixels = nullptr;
  _streamLoadingAtMs = 0;
  return _stream.start(source, _path, _secretAccess, _sourceFileSize, _sourceWidth, _sourceHeight,
                       _drawWidth, _drawHeight, _error);
}

bool ImageView::tickAnimationStream(uint32_t nowMs) {
  String workerError;
  const AnimationStreamWorker::Stats stats = _stream.stats(&workerError);
  if (stats.phase == AnimationStreamWorker::Phase::Error) {
    _error = workerError;
    return false;
  }
  // Finish the frame on its way to the panel before taking another one. A
  // DMA transfer reads the displayed slot, so the loop keeps polling touch
  // and only checks for completion here.
  if (_streamTransferActive) {
    if (_display.dmaImageActive()) {
      logStreamStats(nowMs);
      return true;
    }
    finishStreamTransfer();
  }
  if (_streamPresentActive) {
    tickStreamPresentation();
    logStreamStats(nowMs);
    return true;
  }
  AnimationStreamWorker::Frame frame;
  if (!_stream.takeDueFrame(nowMs, frame)) {
    if (_streamSlot == AnimationStreamWorker::NoSlot) drawStreamLoading(nowMs, stats);
    logStreamStats(nowMs);
    return true;
  }
  if (_streamSlot == AnimationStreamWorker::NoSlot) {
    clearStreamLoading();
    _stream.startTimeline(nowMs - frame.dueMs);
    _imageReady = true;
    _streamPresentedFrames = 0;
    _streamStatsAtMs = nowMs;
    Serial.printf("[Anim] first frame %lu ms after open\n", (unsigned long)(nowMs - _openedAtMs));
  } else {
    // The panel no longer needs the previous slot once the new frame starts.
    _stream.releaseFrame(_streamSlot);
  }
  _streamSlot = frame.slot;
  _streamPixels = frame.pixels;
  _streamPresentedFrames++;
  beginStreamPresentation();
  logStreamStats(nowMs);
  return true;
}

void ImageView::stopAnimationStream() {
  if (_streamSlot != AnimationStreamWorker::NoSlot) logStreamStats(millis(), true);
  const bool streamed = _stream.active();
  // Never free a slot that a DMA transfer is still reading.
  if (_streamTransferActive) finishStreamTransfer();
  _stream.stop();
  // Like the video player, return the LCD_CAM bus and its descriptors.
  if (streamed) _display.endDmaImages();
  _streamDmaFallbackLogged = false;
  _streamSlot = AnimationStreamWorker::NoSlot;
  _streamPixels = nullptr;
  _streamLoadingAtMs = 0;
  _streamPresentActive = false;
  _streamPresentY = 0;
}

uint16_t ImageView::streamFirstVisibleRow() const {
  // Rows under the informative app bar are never sent, so the bar does not
  // flicker as frames change.
  if (_viewMode != ViewMode::Informative || _top >= (int16_t)UiKit::AppBarHeight) return 0;
  const uint16_t hidden = (uint16_t)(UiKit::AppBarHeight - _top);
  return hidden < _drawHeight ? hidden : _drawHeight;
}

void ImageView::beginStreamPresentation() {
  _streamPresentActive = false;
  const uint16_t first = streamFirstVisibleRow();
  if (!_streamPixels || !_display.isReady() || first >= _drawHeight) return;
  const uint16_t *pixels = _streamPixels + (size_t)first * _drawWidth;
  // Slots are 64-byte aligned, so DMA declines only a clipped start row whose
  // offset is not (a width that is not a multiple of four in informative
  // mode), or a panel without LCD_CAM support.
  if (_display.beginDmaImage(_left, _top + first, _drawWidth, _drawHeight - first, pixels)) {
    _streamTransferActive = true;
    return;
  }
  if (!_streamDmaFallbackLogged) {
    Serial.println(F("[Anim] DMA unavailable for this frame; using TFT_eSPI bands"));
    _streamDmaFallbackLogged = true;
  }
  _streamPresentY = first;
  _streamPresentActive = true;
  tickStreamPresentation();
}

void ImageView::finishStreamTransfer() {
  _display.finishDmaImage();
  _streamTransferActive = false;
}

void ImageView::tickStreamPresentation() {
  if (!_streamPresentActive || !_streamPixels || !_display.isReady() || _streamPresentY >= _drawHeight) {
    _streamPresentActive = false;
    return;
  }
  // One band per loop pass lets touch polling observe press and release
  // edges while a frame is sent.
  uint16_t rows = _drawHeight - _streamPresentY;
  if (rows > StreamPresentBandRows) rows = StreamPresentBandRows;
  _display.tft().pushImage(_left, _top + _streamPresentY, _drawWidth, rows,
                           const_cast<uint16_t *>(_streamPixels + (size_t)_streamPresentY * _drawWidth));
  _streamPresentY += rows;
  if (_streamPresentY >= _drawHeight) _streamPresentActive = false;
}

void ImageView::presentStreamFrameNow() {
  if (_streamTransferActive) finishStreamTransfer();
  beginStreamPresentation();
  // A DMA transfer completes in tick(); a banded fallback finishes here.
  while (_streamPresentActive) tickStreamPresentation();
}

void ImageView::drawStreamLoading(uint32_t nowMs, const AnimationStreamWorker::Stats &stats) {
  if (!_display.isReady() || (uint32_t)(nowMs - _openedAtMs) < StreamLoadingDelayMs) return;
  const bool first = _streamLoadingAtMs == 0;
  if (!first && (uint32_t)(nowMs - _streamLoadingDrawnMs) < StreamLoadingRefreshMs) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t centerX = _display.width() / 2;
  const int16_t centerY = _display.height() / 2;
  if (first) {
    _streamLoadingAtMs = nowMs;
    tft.fillRect(centerX - 130, centerY - 26, 260, 52, TFT_BLACK);
    UiText::draw(tft, F("Loading animation"), centerX, centerY - 11, TFT_WHITE, TFT_BLACK,
                 UiText::Size::Compact, MC_DATUM);
  }
  // A WebP source loads into PSRAM before its first frame, so it shows real
  // progress; a GIF shows the moving indeterminate segment.
  const bool determinate = stats.sourceSize != 0;
  const uint8_t percent = determinate
    ? (uint8_t)(((uint64_t)stats.sourceLoaded * 100ULL) / stats.sourceSize) : 0;
  UiKit::drawProgressBar(tft, centerX - 90, centerY + 2, 180, 12, percent, !determinate, TFT_BLUE,
                         TFT_DARKGREY, TFT_BLACK, first, nowMs - _streamLoadingAtMs);
  _streamLoadingDrawnMs = nowMs;
}

void ImageView::clearStreamLoading() {
  if (_streamLoadingAtMs == 0 || !_display.isReady()) return;
  _display.tft().fillRect(_display.width() / 2 - 130, _display.height() / 2 - 26, 260, 52, TFT_BLACK);
  _streamLoadingAtMs = 0;
}

void ImageView::logStreamStats(uint32_t nowMs, bool force) {
  if (_streamSlot == AnimationStreamWorker::NoSlot) return;
  if (!force && (uint32_t)(nowMs - _streamStatsAtMs) < StreamStatsIntervalMs) return;
  _streamStatsAtMs = nowMs;
  const AnimationStreamWorker::Stats stats = _stream.stats();
  if (!force && stats.phase != AnimationStreamWorker::Phase::Running) return;
  Serial.printf("[Anim] dec=%lu pres=%lu late=%lu lagMs=%lu decMs=%lu.%lu/%lu scaleMs=%lu.%lu "
                "psram=%lu/%lu\n",
                (unsigned long)stats.decoded, (unsigned long)_streamPresentedFrames,
                (unsigned long)stats.late, (unsigned long)stats.lagMs,
                (unsigned long)(stats.decodeAverageUs / 1000),
                (unsigned long)((stats.decodeAverageUs / 100) % 10),
                (unsigned long)(stats.decodeMaxUs / 1000), (unsigned long)(stats.scaleAverageUs / 1000),
                (unsigned long)((stats.scaleAverageUs / 100) % 10), (unsigned long)ESP.getFreePsram(),
                (unsigned long)ESP.getMaxAllocPsram());
}

bool ImageView::isGifSource() {
  uint8_t signature[6] = {};
  if (!_file || !lockedSeek(_file, 0) || lockedRead(_file, signature, sizeof(signature)) != sizeof(signature)) return false;
  return FileFormats::fromSignature(signature, sizeof(signature)) == FileFormats::Format::Gif;
}

bool ImageView::isWebpSource() {
  uint8_t signature[12] = {};
  if (!_file || !lockedSeek(_file, 0) || lockedRead(_file, signature, sizeof(signature)) != sizeof(signature)) {
    return false;
  }
  return FileFormats::fromSignature(signature, sizeof(signature)) == FileFormats::Format::Webp;
}

bool ImageView::prepareWebpSource() {
  if (_webpSource) return true;
  if (!_file || _sourceFileSize == 0 || !psramFound()) {
    _error = F("WebP needs PSRAM");
    return false;
  }
  _webpSourceSize = _sourceFileSize;
  _webpSource = (uint8_t *)ps_malloc(_webpSourceSize);
  if (!_webpSource) {
    _error = F("WebP source needs too much PSRAM");
    return false;
  }
  if (!lockedSeek(_file, 0)) {
    _error = F("WebP source seek failed");
    releaseWebpDecoder();
    return false;
  }
  uint32_t offset = 0;
  while (offset < _webpSourceSize) {
    uint16_t amount = (uint16_t)((_webpSourceSize - offset) > CacheChunkBytes
                                   ? CacheChunkBytes : (_webpSourceSize - offset));
    if (lockedRead(_file, _cacheChunk, amount) != amount) {
      _error = F("WebP source read failed");
      releaseWebpDecoder();
      return false;
    }
    memcpy(_webpSource + offset, _cacheChunk, amount);
    offset += amount;
  }
  return true;
}

bool ImageView::inspectWebpSource() {
  if (!prepareWebpSource()) return false;
  WebPBitstreamFeatures features;
  if (WebPGetFeatures(_webpSource, _webpSourceSize, &features) != VP8_STATUS_OK ||
      features.width <= 0 || features.height <= 0) {
    _error = F("Invalid WebP header");
    releaseWebpDecoder();
    return false;
  }
  // open() streams anything whose VP8X header marks an animation, so this
  // still path only meets one through a malformed header.
  if (features.has_animation) {
    _error = F("Invalid WebP animation header");
    releaseWebpDecoder();
    return false;
  }
  _sourceWidth = (uint32_t)features.width;
  _sourceHeight = (uint32_t)features.height;
  return true;
}

void ImageView::scaleWebpRgba(const uint8_t *rgba) {
  if (!_frameBuffer || !rgba || _sourceWidth == 0 || _sourceHeight == 0) return;
  memset(_frameBuffer, 0, (size_t)_display.width() * _display.height() * sizeof(uint16_t));
  for (uint32_t dy = 0; dy < _drawHeight; dy++) {
    uint32_t sourceY = _drawHeight > 1
      ? _sourceCropY + (uint32_t)(((uint64_t)dy * (_sourceCropHeight - 1)) / (_drawHeight - 1))
      : _sourceCropY;
    uint16_t *destination = _frameBuffer +
      (size_t)(_top + dy) * _display.width() + _left;
    const uint8_t *sourceRow = rgba + (size_t)sourceY * _sourceWidth * 4;
    for (uint32_t dx = 0; dx < _drawWidth; dx++) {
      uint32_t sourceX = _drawWidth > 1
        ? _sourceCropX + (uint32_t)(((uint64_t)dx * (_sourceCropWidth - 1)) / (_drawWidth - 1))
        : _sourceCropX;
      const uint8_t *pixel = sourceRow + sourceX * 4;
      uint8_t alpha = pixel[3];
      uint8_t red = (uint8_t)(((uint16_t)pixel[0] * alpha + 127) / 255);
      uint8_t green = (uint8_t)(((uint16_t)pixel[1] * alpha + 127) / 255);
      uint8_t blue = (uint8_t)(((uint16_t)pixel[2] * alpha + 127) / 255);
      destination[dx] = (uint16_t)(((red & 0xf8) << 8) |
                                   ((green & 0xfc) << 3) | (blue >> 3));
    }
  }
}

bool ImageView::decodeWebpImage() {
  if (!_webpSource || !_frameBuffer) return false;
  uint64_t rgbaBytes = (uint64_t)_sourceWidth * _sourceHeight * 4ULL;
  if (rgbaBytes == 0 || rgbaBytes > 0xffffffffULL || _sourceWidth > 0x1fffffffUL) {
    _error = F("WebP dimensions unsupported");
    return false;
  }
  _webpRgba = (uint8_t *)ps_malloc((size_t)rgbaBytes);
  if (!_webpRgba || !WebPDecodeRGBAInto(_webpSource, _webpSourceSize, _webpRgba,
                                        (size_t)rgbaBytes, (int)_sourceWidth * 4)) {
    _error = F("WebP decode failed");
    releaseWebpDecoder();
    return false;
  }
  scaleWebpRgba(_webpRgba);
  releaseWebpDecoder();
  _lastProgress = 100;
  drawLoading(100);
  return true;
}

void ImageView::releaseWebpDecoder() {
  if (_webpRgba) {
    free(_webpRgba);
    _webpRgba = nullptr;
  }
  if (_webpSource) {
    free(_webpSource);
    _webpSource = nullptr;
  }
  _webpSourceSize = 0;
}

bool ImageView::writeCacheHeader() {
  uint8_t header[CacheHeaderSize] = {};
  header[0] = 'E'; header[1] = 'M'; header[2] = '1'; header[3] = '6';
  header[4] = 2; // cache format version
  header[5] = 0; // RGB565 little-endian
  writeLe16(header + 8, CacheWidth);
  writeLe16(header + 10, CacheHeight);
  writeLe32(header + 12, _sourceFileSize);
  writeLe32(header + 16, _sourceWriteTime);
  writeLe32(header + 20, CachePixelBytes);
  writeLe32(header + 24, _sourceWidth);
  writeLe32(header + 28, _sourceHeight);
  return _mediaCache.write(_cacheWriter, header, sizeof(header)) == MediaCache::Result::Complete;
}

void ImageView::startCacheWrite() {
  const FileFormats::Format cacheFormat = _cropZoom ? FileFormats::Format::Em16Zoom
                                                     : FileFormats::Format::Em16;
  const String &cachePath = _cropZoom ? _zoomCachePath : _cachePath;
  const String &cacheTempPath = _cropZoom ? _zoomCacheTempPath : _cacheTempPath;
  if (!_writeCacheAfterRender || _cacheWriting || !_frameBuffer || cachePath.length() == 0) return;
  if (!cacheNeedsRotation() && (_display.width() != CacheWidth || _display.height() != CacheHeight)) {
    Serial.print(F("Image cache: skipped, display is "));
    Serial.print(_display.width());
    Serial.print('x');
    Serial.println(_display.height());
    return;
  }
  Serial.print(F("Image cache: preparing "));
  Serial.println(cachePath);
  CachePaths paths;
  if (!CachePath::forSource(_path, cacheFormat, paths) ||
      _mediaCache.beginWrite(paths, _secretAccess, _cacheWriter) != MediaCache::Result::Complete) {
    Serial.print(F("Image cache: cannot create "));
    Serial.println(paths.directory);
    return;
  }
  if (!writeCacheHeader()) {
    Serial.print(F("Image cache: cannot start "));
    Serial.println(cacheTempPath);
    _mediaCache.abort(_cacheWriter);
    return;
  }
  _cacheOffset = 0;
  _cacheWriting = true;
  Serial.print(F("Image cache: writing "));
  Serial.println(cacheTempPath);
}

void ImageView::writeCacheChunk() {
  if (!_cacheWriting || !_cacheWriter.active || !_frameBuffer) return;
  if (_cacheOffset == CachePixelBytes) {
    finishCacheWrite(true);
    return;
  }
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
  const MediaCache::Result writeResult = _mediaCache.write(_cacheWriter, _cacheChunk, amount);
  if (writeResult == MediaCache::Result::Busy) return;
  if (writeResult != MediaCache::Result::Complete) {
    Serial.print(F("Image cache: write failed "));
    Serial.println(_cropZoom ? _zoomCacheTempPath : _cacheTempPath);
    finishCacheWrite(false);
    return;
  }
  _cacheOffset += amount;
  if (_cacheOffset == CachePixelBytes) finishCacheWrite(true);
}

bool ImageView::cacheNeedsRotation() const {
  return _display.width() == CacheHeight && _display.height() == CacheWidth;
}

void ImageView::finishCacheWrite(bool success) {
  bool committed = false;
  if (success) {
    const MediaCache::Result result = _mediaCache.commit(_cacheWriter);
    if (result == MediaCache::Result::Busy) return;
    committed = result == MediaCache::Result::Complete;
  }
  if (!committed) {
    _mediaCache.abort(_cacheWriter);
    if (success) {
      Serial.print(F("Image cache: commit failed "));
      Serial.println(_cropZoom ? _zoomCachePath : _cachePath);
    }
  }
  _cacheWriting = false;
  _cacheOffset = 0;
  if (committed) {
    Serial.print(F("Image cache ready: "));
    Serial.println(_cropZoom ? _zoomCachePath : _cachePath);
  }
}

bool ImageView::prepareRender() {
  if (!_display.isReady() || _sourceWidth == 0 || _sourceHeight == 0) {
    _error = F("Invalid image size");
    return false;
  }
  uint32_t screenWidth = _display.width();
  uint32_t screenHeight = _display.height();
  if (_cropZoom) {
    _drawWidth = (uint16_t)screenWidth;
    _drawHeight = (uint16_t)screenHeight;
    _sourceCropX = 0;
    _sourceCropY = 0;
    _sourceCropWidth = _sourceWidth;
    _sourceCropHeight = _sourceHeight;
    if ((uint64_t)screenWidth * _sourceHeight > (uint64_t)screenHeight * _sourceWidth) {
      _sourceCropHeight = (uint32_t)(((uint64_t)_sourceWidth * screenHeight) / screenWidth);
      if (_sourceCropHeight == 0) _sourceCropHeight = 1;
      _sourceCropY = (_sourceHeight - _sourceCropHeight) / 2;
    } else {
      _sourceCropWidth = (uint32_t)(((uint64_t)_sourceHeight * screenWidth) / screenHeight);
      if (_sourceCropWidth == 0) _sourceCropWidth = 1;
      _sourceCropX = (_sourceWidth - _sourceCropWidth) / 2;
    }
  } else if ((uint64_t)screenWidth * _sourceHeight <= (uint64_t)screenHeight * _sourceWidth) {
    _drawWidth = screenWidth;
    _drawHeight = (uint16_t)(((uint64_t)screenWidth * _sourceHeight) / _sourceWidth);
  } else {
    _drawHeight = screenHeight;
    _drawWidth = (uint16_t)(((uint64_t)screenHeight * _sourceWidth) / _sourceHeight);
  }
  if (!_cropZoom) {
    _sourceCropX = 0;
    _sourceCropY = 0;
    _sourceCropWidth = _sourceWidth;
    _sourceCropHeight = _sourceHeight;
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
      ? _sourceCropX + (uint32_t)(((uint64_t)dx * (_sourceCropWidth - 1) << 16) /
                                  (_drawWidth - 1))
      : _sourceCropX;
  }
  return true;
}

bool ImageView::decodeRasterImage() {
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
  } else if (_format == ImageFormat::Webp) {
    return decodeWebpImage();
  }
  _lastProgress = 100;
  drawLoading(100);
  return true;
}

void ImageView::storeRasterPixel(uint32_t sourceX, uint32_t sourceY, uint16_t color) {
  if (!_frameBuffer || sourceX >= _sourceWidth || sourceY >= _sourceHeight ||
      sourceX < _sourceCropX || sourceY < _sourceCropY ||
      sourceX >= _sourceCropX + _sourceCropWidth ||
      sourceY >= _sourceCropY + _sourceCropHeight) return;
  uint32_t cropX = sourceX - _sourceCropX;
  uint32_t cropY = sourceY - _sourceCropY;
  uint32_t dx0 = (uint32_t)(((uint64_t)cropX * _drawWidth) / _sourceCropWidth);
  uint32_t dx1 = (uint32_t)((((uint64_t)(cropX + 1) * _drawWidth) + _sourceCropWidth - 1) /
                             _sourceCropWidth);
  uint32_t dy0 = (uint32_t)(((uint64_t)cropY * _drawHeight) / _sourceCropHeight);
  uint32_t dy1 = (uint32_t)((((uint64_t)(cropY + 1) * _drawHeight) + _sourceCropHeight - 1) /
                             _sourceCropHeight);
  if (dx1 <= dx0) dx1 = dx0 + 1;
  if (dy1 <= dy0) dy1 = dy0 + 1;
  if (dx1 > _drawWidth) dx1 = _drawWidth;
  if (dy1 > _drawHeight) dy1 = _drawHeight;
  for (uint32_t dy = dy0; dy < dy1; dy++) {
    uint16_t *row = _frameBuffer + (size_t)(_top + dy) * _display.width() + _left;
    for (uint32_t dx = dx0; dx < dx1; dx++) row[dx] = color;
  }
}

void *ImageView::pngOpen(const char *name, int32_t *size) {
  if (!_decoderOwner) return nullptr;
  _decoderOwner->_decoderFile = _decoderOwner->openSource(name);
  if (!_decoderOwner->_decoderFile) return nullptr;
  *size = (int32_t)_decoderOwner->_decoderFile.size();
  return &_decoderOwner->_decoderFile;
}

void ImageView::pngClose(void *handle) {
  StorageFile *file = static_cast<StorageFile *>(handle);
  if (file) file->close();
}

int32_t ImageView::pngRead(PNGFILE *file, uint8_t *buffer, int32_t length) {
  if (!file || !file->fHandle) return 0;
  StorageFile *source = static_cast<StorageFile *>(file->fHandle);
  return lockedRead(*source, buffer, length);
}

int32_t ImageView::pngSeek(PNGFILE *file, int32_t position) {
  if (!file || !file->fHandle || position < 0) return 0;
  StorageFile *source = static_cast<StorageFile *>(file->fHandle);
  return lockedSeek(*source, position) ? position : 0;
}

void *ImageView::jpegOpen(const char *name, int32_t *size) { return pngOpen(name, size); }
void ImageView::jpegClose(void *handle) { pngClose(handle); }
int32_t ImageView::jpegRead(JPEGFILE *file, uint8_t *buffer, int32_t length) {
  if (!file || !file->fHandle || length <= 0) return 0;
  return lockedRead(*static_cast<StorageFile *>(file->fHandle), buffer, length);
}
int32_t ImageView::jpegSeek(JPEGFILE *file, int32_t position) {
  if (!file || !file->fHandle || position < 0) return 0;
  return lockedSeek(*static_cast<StorageFile *>(file->fHandle), position) ? position : 0;
}

int ImageView::pngDraw(PNGDRAW *draw) {
  if (!_decoderOwner || !draw || draw->iWidth <= 0) return 0;
  ImageView &viewer = *_decoderOwner;
  // PNGdec can expand the final packed/SIMD group beyond the visible width.
  // The converter needs the padding, while the loop below uses only iWidth.
  uint16_t *pixels = (uint16_t *)malloc(((size_t)draw->iWidth + 7) * sizeof(uint16_t));
  if (!pixels) return 0;
  viewer._png.getLineAsRGB565(draw, pixels, PNG_RGB565_LITTLE_ENDIAN, 0);
  for (int x = 0; x < draw->iWidth; x++) viewer.storeRasterPixel((uint32_t)x, (uint32_t)draw->y, pixels[x]);
  free(pixels);
  viewer.updateDecodeProgress((uint8_t)(((uint32_t)(draw->y + 1) * 100) / viewer._sourceHeight));
  return 1;
}

int ImageView::jpegDraw(JPEGDRAW *draw) {
  if (!_decoderOwner || !draw || !draw->pPixels) return 0;
  ImageView &viewer = *_decoderOwner;
  for (int y = 0; y < draw->iHeight; y++) {
    for (int x = 0; x < draw->iWidthUsed; x++) {
      viewer.storeRasterPixel((uint32_t)(draw->x + x), (uint32_t)(draw->y + y), draw->pPixels[y * draw->iWidth + x]);
    }
  }
  viewer.updateDecodeProgress((uint8_t)(((uint32_t)(draw->y + draw->iHeight) * 100) / viewer._sourceHeight));
  return 1;
}

bool ImageView::renderRows(uint8_t maxRows) {
  if (!_rendering && _nextDrawY < 0) return true;
  uint8_t done = 0;
  while (done < maxRows && _nextDrawY >= 0) {
    uint32_t dy = (uint32_t)_nextDrawY;
    uint32_t y16 = _drawHeight > 1
      ? _sourceCropY + (uint32_t)(((uint64_t)dy * (_sourceCropHeight - 1) << 16) /
                                   (_drawHeight - 1))
      : _sourceCropY;
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
    _imageReady = true;
    presentStaticFrame();
    if (_writeCacheAfterRender) startCacheWrite();
    releaseRenderScratch();
  }
  return true;
}

bool ImageView::readSourceRow(uint32_t sourceY, uint8_t *target) {
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

bool ImageView::readUncompressedRow(uint32_t sourceY, uint8_t *target) {
  uint32_t fileRow = _topDown ? sourceY : _sourceHeight - 1 - sourceY;
  uint64_t offset = (uint64_t)_pixelOffset + (uint64_t)fileRow * _rowStride;
  if (offset > 0xffffffffULL || !lockedSeek(_file, (uint32_t)offset) ||
      lockedRead(_file, _rawRow, _rowStride) != _rowStride) return false;
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

bool ImageView::readRleByte(uint8_t &value) {
  return lockedRead(_file, &value, 1) == 1;
}

bool ImageView::readRleRow(uint8_t *target) {
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

void ImageView::setPixel(uint8_t *row, uint32_t x, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  if (x >= _sourceWidth) return;
  if (a != 255) {
    r = (uint8_t)(((uint16_t)r * a) / 255);
    g = (uint8_t)(((uint16_t)g * a) / 255);
    b = (uint8_t)(((uint16_t)b * a) / 255);
  }
  uint8_t *p = row + x * 4;
  p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
}

uint8_t ImageView::extractChannel(uint32_t value, uint32_t mask, uint8_t shift) const {
  if (!mask) return 0;
  uint32_t raw = (value & mask) >> shift;
  uint32_t max = mask >> shift;
  return (uint8_t)((raw * 255UL + max / 2) / max);
}

uint16_t ImageView::blendPixel(const uint8_t *row0,
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

void ImageView::releaseRenderScratch() {
  if (_xCoordinates) free(_xCoordinates);
  if (_rawRow) free(_rawRow);
  if (_row0) free(_row0);
  if (_row1) free(_row1);
  _xCoordinates = nullptr;
  _rawRow = nullptr;
  _row0 = nullptr;
  _row1 = nullptr;
}

void ImageView::releaseBuffers() {
  if (_frameBuffer) free(_frameBuffer);
  _frameBuffer = nullptr;
  releaseRenderScratch();
}

uint16_t ImageView::readLe16(const uint8_t *buffer) const {
  return (uint16_t)buffer[0] | ((uint16_t)buffer[1] << 8);
}

uint32_t ImageView::readLe32(const uint8_t *buffer) const {
  return (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
         ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
}

int32_t ImageView::readLe32Signed(const uint8_t *buffer) const {
  return (int32_t)readLe32(buffer);
}

void ImageView::writeLe16(uint8_t *buffer, uint16_t value) const {
  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
}

void ImageView::writeLe32(uint8_t *buffer, uint32_t value) const {
  buffer[0] = (uint8_t)value;
  buffer[1] = (uint8_t)(value >> 8);
  buffer[2] = (uint8_t)(value >> 16);
  buffer[3] = (uint8_t)(value >> 24);
}
