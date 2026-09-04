#include "ImageViewerScreen.h"

#include "AnimationCache.h"
#include "FileFormats.h"
#include "FilesScreen.h"

// Arduino's sketch builder does not compile sources below the sketch root;
// include the pinned, header-based decoder implementation exactly once.
#include "third_party/AnimatedGIF/AnimatedGIF.cpp"

#include "Navigator.h"
#include "UiKit.h"

#include <SD.h>
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include "SpiBusLock.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {
const uint32_t InvalidSourceRow = 0xffffffffUL;
const uint32_t MaxSupportedDibSize = 124;
// Version 1 used the same display-ready pixels, but its large-animation path
// never enabled the second-core ring loader. Bump the cache version so old
// files are rebuilt with the corrected playback contract.
}

ImageViewerScreen *ImageViewerScreen::_decoderOwner = nullptr;

ImageViewerScreen::ImageViewerScreen(Navigator &navigator,
                                     TouchController &touch,
                                     DisplayController &display,
                                     SdCardController &sdCard,
                                     SecretStorage &secretStorage)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _secretStorage(secretStorage),
    _secretAccess(false),
    _webpSource(nullptr),
    _webpSourceSize(0),
    _webpRgba(nullptr),
    _webpAnimDecoder(nullptr),
    _webpAnimInfo{},
    _webpAnimated(false),
    _webpPreviousTimestamp(0),
    _previousImagePath(),
    _nextImagePath(),
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
    _gifState(GifState::None),
    _gifDecoderOpen(false),
    _gifBuildWritePending(false),
    _gifBuildLastFrame(false),
    _gifBuildFrame(0),
    _gifBuildWriteOffset(0),
    _gifBuildDelay(0),
    _gifFrameCount(0),
    _gifPlaybackFrame(0),
    _gifPlaybackDelay(0),
    _gifNextDelay(0),
    _gifCyclesCompleted(0),
    _gifLoopCount(-1),
    _gifPlaybackA(nullptr),
    _gifPlaybackB(nullptr),
    _gifBufferedFrame(0),
    _gifBufferedOffset(0),
    _gifBufferedLoading(false),
    _gifResidentFrames(nullptr),
    _gifResidentDelays(nullptr),
    _gifResidentSequences(nullptr),
    _gifResidentStates(nullptr),
    _gifResidentCapacity(0),
    _gifPreloadTarget(0),
    _gifPreloadSlot(0),
    _gifPreloadSequence(0),
    _gifPreloadOffset(0),
    _gifNextLoadSequence(0),
    _gifPlaybackSequence(0),
    _gifFullResident(false),
    _gifPreloadActive(false),
    _gifLoadRequests(nullptr),
    _gifLoadResults(nullptr),
    _gifLoaderStopped(nullptr),
    _gifLoaderTask(nullptr),
    _gifLoaderStop(false),
    _gifLoaderError(false),
    _gifLoadedFrames(0),
    _gifUnderruns(0),
    _gifDroppedFrames(0),
    _gifWaitingForFrame(false),
    _gifActiveIsA(true),
    _gifNextLoaded(false),
    _gifNextFrameAt(0),
    _gifCanvasPixels(0),
    _gifPresentFrame(nullptr),
    _gifPresentY(0),
    _gifPresentActive(false),
    _lastTouchDown(false),
    _lastProgress(0),
    _rleFileRow(0),
    _rleX(0),
    _rlePendingBlankRows(0),
    _rleDone(false) {
  // reset() releases any decoder-owned buffers, including on the first
  // screen entry. Initialize the decoder before that first release so its
  // frame-buffer pointer is null rather than indeterminate.
  _gif.begin(GIF_PALETTE_RGB565_LE);
}

void ImageViewerScreen::onEnter(const ScreenData &data) {
  reset();
  _lastTouchDown = false;
  _secretAccess = data.fileAccess == FileAccessMode::Secret;
  if (data.kind != ScreenDataKind::ImagePath || data.text.length() == 0) {
    _error = F("No image selected");
    drawError();
    return;
  }
  _path = data.text;
  _previousImagePath = data.previousImagePath;
  _nextImagePath = data.nextImagePath;
  // Put a real frame on the panel before touching SD or allocating decode
  // buffers.  Animated formats continue their build incrementally in tick(),
  // and this prevents a blank/previous Files screen during header/cache work.
  draw();
  drawLoading(0);
  yield();
  if (!_sdCard.isMounted()) {
    _error = F("SD card unavailable");
    drawError();
    return;
  }
  _file = _secretStorage.open(_path, FILE_READ, _secretAccess);
  if (!_file) {
    _error = F("Unable to open source image");
    drawError();
    return;
  }
  _sourceFileSize = (uint32_t)_file.size();
  _sourceWriteTime = (uint32_t)_file.getLastWrite();
  if (isGifSource()) {
    _format = ImageFormat::Gif;
    _cachePath = animationCachePathForSource();
    _cacheTempPath = _cachePath + F(".tmp");
    if (loadAnimationCache()) return;
  } else if (isWebpSource()) {
    _format = ImageFormat::Webp;
    if (!inspectWebpSource()) {
      if (_error.length() == 0) _error = F("Invalid WebP image");
      drawError();
      return;
    }
    _cachePath = _webpAnimated ? animationCachePathForSource() : cachePathForSource();
    _cacheTempPath = _cachePath + F(".tmp");
    if (_webpAnimated && loadAnimationCache()) return;
  } else {
    _cachePath = cachePathForSource();
    _cacheTempPath = _cachePath + F(".tmp");
  }
  if (loadCache()) {
    Serial.print(F("Image cache hit: "));
    Serial.println(_cachePath);
    draw();
    _display.tft().pushImage(0, 0, _display.width(), _display.height(), _frameBuffer);
    releaseBuffers();
    releaseWebpDecoder();
    drawCloseIcon();
    return;
  }
  if (!readRasterHeader() || !prepareRender()) {
    if (_error.length() == 0) _error = F("Unable to render image");
    releaseWebpDecoder();
    drawError();
    return;
  }
  draw();
  drawLoading(0);
  if (_format == ImageFormat::Gif || (_format == ImageFormat::Webp && _webpAnimated)) {
    bool started = _format == ImageFormat::Gif ? startGifBuild() : startWebpBuild();
    if (!started) {
      if (_error.length() == 0) _error = F("Could not cache animation");
      if (_cacheFile) _cacheFile.close();
      if (_cacheTempPath.length() > 0) _secretStorage.remove(_cacheTempPath);
      releaseGifDecoder();
      releaseWebpDecoder();
      releaseGifBuffers();
      releaseBuffers();
      drawError();
      return;
    }
    drawCloseIcon();
    return;
  }
  if (_format != ImageFormat::Bmp && !decodeRasterImage()) {
    if (_error.length() == 0) _error = F("Could not decode image");
    releaseBuffers();
    releaseWebpDecoder();
    drawError();
    return;
  }
  _rendering = _format == ImageFormat::Bmp;
  if (!_rendering) {
    _display.tft().pushImage(0, 0, _display.width(), _display.height(), _frameBuffer);
    startCacheWrite();
    if (!_cacheWriting) {
      releaseBuffers();
      releaseWebpDecoder();
    }
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
  if (_gifState == GifState::Building) {
    bool built = _format == ImageFormat::Webp ? buildWebpFrame() : buildGifFrame();
    if (!built) {
      _gifState = GifState::None;
      if (_error.length() == 0) _error = F("Could not cache animation");
      if (_cacheFile) _cacheFile.close();
      if (_cacheTempPath.length() > 0) _secretStorage.remove(_cacheTempPath);
      releaseGifDecoder();
      releaseWebpDecoder();
      releaseGifBuffers();
      releaseBuffers();
      drawError();
    }
    return;
  }
  if (_gifState == GifState::Preloading) {
    if (!tickGifPreload()) {
      _gifState = GifState::None;
      if (_error.length() == 0) _error = F("GIF preload failed");
      releaseGifBuffers();
      drawError();
    }
    return;
  }
  if (_gifState == GifState::Playing) {
    if (!tickGifPlayback(nowMs)) {
      _gifState = GifState::None;
      if (_error.length() == 0) _error = F("GIF playback error");
      releaseGifBuffers();
      drawError();
    } else if (_gifState == GifState::None) {
      releaseGifBuffers();
      if (_gifCacheReadFile) _gifCacheReadFile.close();
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

void ImageViewerScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;
  // The full 48 px app-bar navigation target remains responsive while an
  // animation is sharing the SPI bus.
  if (point.x < CloseTapSize && point.y < CloseTapSize) {
    _navigator.pop();
    return;
  }

  String target;
  if (point.x < EdgeTapWidth) {
    target = _previousImagePath;
  } else if (point.x >= _display.width() - EdgeTapWidth) {
    target = _nextImagePath;
  }
  if (target.length() == 0) return;

  FilesScreen *files = static_cast<FilesScreen *>(_navigator.screenFor(ScreenId::Files));
  ScreenData data;
  if (files && files->imageViewerDataFor(target, data)) {
    _navigator.replace(ScreenId::ImageViewer, data);
  }
}

void ImageViewerScreen::reset() {
  bool removeGifTemp = _gifState == GifState::Building;
  _rendering = false;
  _gifState = GifState::None;
  stopGifLoader();
  _decoderOwner = nullptr;
  releaseGifDecoder();
  releaseWebpDecoder();
  if (_file) _file.close();
  if (_decoderFile) _decoderFile.close();
  if (_cacheFile) _cacheFile.close();
  if (_gifCacheReadFile) _gifCacheReadFile.close();
  if ((_cacheWriting || removeGifTemp) && _cacheTempPath.length() > 0) _secretStorage.remove(_cacheTempPath);
  _cacheWriting = false;
  releaseGifBuffers();
  releaseBuffers();
  _path = "";
  _error = "";
  _cachePath = "";
  _cacheTempPath = "";
  _sourceFileSize = 0;
  _sourceWriteTime = 0;
  _cacheOffset = 0;
  _gifBuildWritePending = false;
  _gifBuildLastFrame = false;
  _gifBuildFrame = 0;
  _gifBuildWriteOffset = 0;
  _gifBuildDelay = 0;
  _webpPreviousTimestamp = 0;
  _gifFrameCount = 0;
  _gifPlaybackFrame = 0;
  _gifPlaybackDelay = 0;
  _gifNextDelay = 0;
  _gifBufferedFrame = 0;
  _gifBufferedOffset = 0;
  _gifBufferedLoading = false;
  _gifCyclesCompleted = 0;
  _gifLoopCount = -1;
  _gifResidentCapacity = 0;
  _gifPreloadTarget = 0;
  _gifPreloadSlot = 0;
  _gifPreloadSequence = 0;
  _gifPreloadOffset = 0;
  _gifNextLoadSequence = 0;
  _gifPlaybackSequence = 0;
  _gifFullResident = false;
  _gifPreloadActive = false;
  _gifLoaderStop = false;
  _gifLoaderError = false;
  _gifLoadedFrames = 0;
  _gifUnderruns = 0;
  _gifDroppedFrames = 0;
  _gifWaitingForFrame = false;
  _gifActiveIsA = true;
  _gifNextLoaded = false;
  _gifNextFrameAt = 0;
  _gifCanvasPixels = 0;
  _gifPresentFrame = nullptr;
  _gifPresentY = 0;
  _gifPresentActive = false;
  _format = ImageFormat::Unknown;
  _previousImagePath = "";
  _nextImagePath = "";
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
  tft.fillRect(0, 0, UiKit::AppBarHeight, UiKit::AppBarHeight, TFT_BLACK);
  // The image viewer deliberately remains full-screen; draw only the kit's close affordance.
  UiKit::drawTopAppBar(tft, UiKit::AppBarHeight, String(), UiKit::NavigationIcon::Close);
}

void ImageViewerScreen::drawError() {
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
  String filename = _path;
  int slash = filename.lastIndexOf('/');
  if (slash >= 0) filename = filename.substring(slash + 1);
  if (filename.length() > 28) filename = String(F("...")) + filename.substring(filename.length() - 25);
  UiText::draw(tft, filename, centerX, centerY + 36, TFT_LIGHTGREY, TFT_BLACK,
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
  drawCloseIcon();
}

const char *ImageViewerScreen::formatName() const {
  switch (_format) {
    case ImageFormat::Bmp: return "BMP";
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Jpeg: return "JPEG";
    case ImageFormat::Gif: return "GIF";
    case ImageFormat::Webp: return "WebP";
    default: return "unknown";
  }
}

void ImageViewerScreen::drawLoading(uint8_t percent) {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  int16_t centerX = _display.width() / 2;
  int16_t centerY = _display.height() / 2;
  tft.fillRect(centerX - 100, centerY - 28, 200, 58, TFT_BLACK);
  String progress = String(percent) + F("%");
  UiText::draw(tft, F("Loading image"), centerX, centerY - 12, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  UiText::draw(tft, progress, centerX, centerY + 14, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
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
  uint8_t signature[12] = {};
  if (!_file.seek(0) || _file.read(signature, sizeof(signature)) != sizeof(signature)) {
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
    int jpegResult = _file.seek(0) ? _jpeg.open(_path.c_str(), jpegOpen, jpegClose, jpegRead, jpegSeek, jpegDraw) : 0;
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
  if (sourceFormat == FileFormats::Format::Gif) {
    _format = ImageFormat::Gif;
    _decoderOwner = this;
    _gif.begin(GIF_PALETTE_RGB565_LE);
    // A null draw callback makes AnimatedGIF retain the complete cooked
    // RGB565 canvas in its frame buffer for scaling and caching below.
    if (!_gif.open(_path.c_str(), gifOpen, gifClose, gifRead, gifSeek, nullptr)) {
      _error = String(F("GIF header error ")) + _gif.getLastError();
      _decoderOwner = nullptr;
      if (_decoderFile) _decoderFile.close();
      return false;
    }
    _gifDecoderOpen = true;
    _sourceWidth = (uint32_t)_gif.getCanvasWidth();
    _sourceHeight = (uint32_t)_gif.getCanvasHeight();
    if (_sourceWidth == 0 || _sourceHeight == 0) {
      _error = F("GIF has invalid dimensions");
      releaseGifDecoder();
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

String ImageViewerScreen::cachePathForSource() const {
  int slash = _path.lastIndexOf('/');
  String directory = slash <= 0 ? F("/") : _path.substring(0, slash);
  String name = slash < 0 ? _path : _path.substring(slash + 1);
  return directory + (directory == F("/") ? F("") : F("/")) + F(".cache/") + name + F(".em16");
}

String ImageViewerScreen::animationCachePathForSource() const {
  int slash = _path.lastIndexOf('/');
  String directory = slash <= 0 ? F("/") : _path.substring(0, slash);
  String name = slash < 0 ? _path : _path.substring(slash + 1);
  return directory + (directory == F("/") ? F("") : F("/")) + F(".cache/") + name + F(".emga");
}

bool ImageViewerScreen::readCacheHeader(StorageFile &file) const {
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
  // JPG, PNG, and BMP intentionally probe their .em16 cache before parsing
  // the source header, so their format is still Unknown here.  Only animated
  // sources use the separate .emga cache path.
  if (_format == ImageFormat::Gif ||
      (_format == ImageFormat::Webp && _webpAnimated)) return false;
  if (!cacheNeedsRotation() && (_display.width() != CacheWidth || _display.height() != CacheHeight)) return false;
  if (!_secretStorage.exists(_cachePath)) return false;
  StorageFile cache = _secretStorage.open(_cachePath, FILE_READ, _secretAccess);
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

bool ImageViewerScreen::writeAnimationCacheHeader(StorageFile &file,
                                                   uint32_t frameCount,
                                                   int32_t loopCount) const {
  return AnimationCache::writeHeader(file, _sourceFileSize, _sourceWriteTime,
                                     _sourceWidth, _sourceHeight, frameCount,
                                     loopCount, _drawWidth, _drawHeight);
}

bool ImageViewerScreen::readAnimationCacheHeader(StorageFile &file,
                                                  uint32_t &frameCount,
                                                  int32_t &loopCount,
                                                  uint32_t &sourceWidth,
                                                  uint32_t &sourceHeight,
                                                  uint16_t &drawWidth,
                                                  uint16_t &drawHeight) const {
  if (!file || file.size() < AnimationCacheHeaderSize) return false;
  uint8_t header[AnimationCacheHeaderSize];
  if (!file.seek(0) || file.read(header, sizeof(header)) != sizeof(header)) return false;
  if (readLe32(header + 0) != AnimationCache::Magic || header[4] != AnimationCache::Version ||
      header[5] != 2 || readLe16(header + 8) != CacheWidth ||
      readLe16(header + 10) != CacheHeight ||
      readLe32(header + 12) != _sourceFileSize ||
      readLe32(header + 16) != _sourceWriteTime) return false;
  sourceWidth = readLe32(header + 20);
  sourceHeight = readLe32(header + 24);
  frameCount = readLe32(header + 28);
  loopCount = (int32_t)readLe32(header + 32);
  drawWidth = readLe16(header + 36);
  drawHeight = readLe16(header + 38);
  if (sourceWidth == 0 || sourceHeight == 0 || frameCount == 0 ||
      drawWidth == 0 || drawHeight == 0 ||
      drawWidth > GifPhotoMaxWidth || drawHeight > GifPhotoMaxHeight) return false;
  uint64_t pixels = (uint64_t)drawWidth * drawHeight;
  uint64_t recordBytes = AnimationFrameHeaderSize + (pixels * 3 + 1) / 2;
  uint64_t expected = AnimationCacheHeaderSize + recordBytes * frameCount;
  return expected == file.size();
}

bool ImageViewerScreen::loadAnimationCache() {
  if (!psramFound() || !_secretStorage.exists(_cachePath)) return false;
  StorageFile cache = _secretStorage.open(_cachePath, FILE_READ, _secretAccess);
  if (!cache) return false;
  uint32_t frameCount = 0, sourceWidth = 0, sourceHeight = 0;
  uint16_t drawWidth = 0, drawHeight = 0;
  int32_t loopCount = -1;
  bool valid = readAnimationCacheHeader(cache, frameCount, loopCount, sourceWidth, sourceHeight,
                                        drawWidth, drawHeight);
  if (!valid) {
    cache.close();
    return false;
  }
  _sourceWidth = sourceWidth;
  _sourceHeight = sourceHeight;
  _drawWidth = drawWidth;
  _drawHeight = drawHeight;
  _left = (int16_t)((_display.width() - _drawWidth) / 2);
  _top = (int16_t)((_display.height() - _drawHeight) / 2);
  _gifFrameCount = frameCount;
  _gifLoopCount = loopCount;
  _gifCacheReadFile = cache;
  if (!startGifPlayback()) {
    _gifCacheReadFile.close();
    releaseGifBuffers();
    return false;
  }
  if (_format == ImageFormat::Webp) releaseWebpDecoder();
  Serial.print(_format == ImageFormat::Webp ? F("Animated WebP cache hit: ")
                                             : F("Animated GIF cache hit: "));
  Serial.println(_cachePath);
  return true;
}

bool ImageViewerScreen::startGifBuild() {
  if (_format != ImageFormat::Gif || !_gifDecoderOpen || !_frameBuffer) return false;
  if (!cacheNeedsRotation() &&
      (_display.width() != CacheWidth || _display.height() != CacheHeight)) {
    _error = F("GIF cache does not support this display size");
    return false;
  }
  int slash = _cachePath.lastIndexOf('/');
  String directory = slash <= 0 ? F("/") : _cachePath.substring(0, slash);
  if (!_secretStorage.mkdir(directory)) {
    _error = F("Cannot create GIF cache directory");
    return false;
  }
  _secretStorage.remove(_cacheTempPath);
  _cacheFile = _secretStorage.open(_cacheTempPath, FILE_WRITE, _secretAccess);
  if (!_cacheFile || !writeAnimationCacheHeader(_cacheFile, 0, _gif.getLoopCount())) {
    _error = F("Cannot start GIF cache");
    if (_cacheFile) _cacheFile.close();
    _secretStorage.remove(_cacheTempPath);
    return false;
  }
  _gifLoopCount = _gif.getLoopCount();
  _gifBuildFrame = 0;
  _gifBuildWriteOffset = 0;
  _gifBuildWritePending = false;
  _gifBuildLastFrame = false;
  _gifState = GifState::Building;
  drawGifLoading(0);
  return true;
}

bool ImageViewerScreen::buildGifFrame() {
  if (!_cacheFile || !_gifDecoderOpen || !_frameBuffer) return false;
  if (_gifBuildWritePending) {
    uint32_t recordBytes = gifFrameRecordBytes();
    uint32_t remaining = recordBytes - _gifBuildWriteOffset;
    uint16_t amount = remaining > CacheChunkBytes ? CacheChunkBytes : (uint16_t)remaining;
    if (_gifBuildWriteOffset < AnimationFrameHeaderSize) {
      uint8_t delay[AnimationFrameHeaderSize] = {};
      writeLe32(delay, _gifBuildDelay);
      uint16_t available = AnimationFrameHeaderSize - _gifBuildWriteOffset;
      if (amount > available) amount = available;
      memcpy(_cacheChunk, delay + _gifBuildWriteOffset, amount);
    } else {
      uint32_t payloadOffset = _gifBuildWriteOffset - AnimationFrameHeaderSize;
      for (uint16_t i = 0; i < amount; i++) {
        uint32_t position = payloadOffset + i;
        uint32_t pair = position / 3;
        uint32_t firstPixel = pair * 2;
        uint16_t x0 = (uint16_t)(firstPixel % _drawWidth);
        uint16_t y0 = (uint16_t)(firstPixel / _drawWidth);
        uint16_t first = _frameBuffer[(size_t)(_top + y0) * _display.width() + _left + x0];
        uint16_t rgb444a = (uint16_t)((first & 0xf000) >> 4) |
                           (uint16_t)((first & 0x0780) >> 3) |
                           (uint16_t)((first & 0x001e) >> 1);
        uint16_t rgb444b = 0;
        if (firstPixel + 1 < (uint32_t)_drawWidth * _drawHeight) {
          uint16_t x1 = (uint16_t)((firstPixel + 1) % _drawWidth);
          uint16_t y1 = (uint16_t)((firstPixel + 1) / _drawWidth);
          uint16_t second = _frameBuffer[(size_t)(_top + y1) * _display.width() + _left + x1];
          rgb444b = (uint16_t)((second & 0xf000) >> 4) |
                    (uint16_t)((second & 0x0780) >> 3) |
                    (uint16_t)((second & 0x001e) >> 1);
        }
        switch (position % 3) {
          case 0: _cacheChunk[i] = (uint8_t)(rgb444a >> 4); break;
          case 1: _cacheChunk[i] = (uint8_t)((rgb444a << 4) | (rgb444b >> 8)); break;
          default: _cacheChunk[i] = (uint8_t)rgb444b; break;
        }
      }
    }
    if (_cacheFile.write(_cacheChunk, amount) != amount) {
      _error = F("GIF cache write failed");
      return false;
    }
    _gifBuildWriteOffset += amount;
    if (_gifBuildWriteOffset == recordBytes) {
      _gifBuildWritePending = false;
      _gifBuildFrame++;
      uint8_t percent = 0;
      if (_sourceFileSize != 0 && _decoderFile) {
        uint32_t sourcePosition = (uint32_t)_decoderFile.position();
        uint32_t calculated = (uint32_t)(((uint64_t)sourcePosition * 100UL) / _sourceFileSize);
        percent = (uint8_t)(calculated > 99 ? 99 : calculated);
      }
      if (_gifBuildLastFrame) percent = 100;
      drawGifLoading(percent);
      if (_gifBuildLastFrame) return finishGifBuild();
    }
    return true;
  }

  int delay = 0;
  int result = _gif.playFrame(false, &delay, nullptr);
  if (result < 0 || _gif.getLastError() != GIF_SUCCESS) {
    _error = String(F("GIF decode error ")) + _gif.getLastError();
    return false;
  }
  _gifBuildDelay = (uint32_t)(delay < (int)GifMinimumDelayMs ? GifMinimumDelayMs : delay);
  scaleGifFrame();
  _gifBuildLastFrame = result == 0;
  _gifBuildWriteOffset = 0;
  _gifBuildWritePending = true;
  return true;
}

bool ImageViewerScreen::finishGifBuild() {
  if (!_cacheFile.seek(0) || !writeAnimationCacheHeader(_cacheFile, _gifBuildFrame, _gifLoopCount)) {
    _error = F("GIF cache header write failed");
    return false;
  }
  _cacheFile.close();
  if (_secretStorage.exists(_cachePath)) _secretStorage.remove(_cachePath);
  if (!_secretStorage.rename(_cacheTempPath, _cachePath)) {
    _error = F("GIF cache commit failed");
    _secretStorage.remove(_cacheTempPath);
    return false;
  }
  releaseGifDecoder();
  releaseBuffers();
  _gifCacheReadFile = _secretStorage.open(_cachePath, FILE_READ, _secretAccess);
  if (!_gifCacheReadFile) {
    _error = F("GIF cache reopen failed");
    return false;
  }
  _gifFrameCount = _gifBuildFrame;
  if (!startGifPlayback()) {
    _gifCacheReadFile.close();
    releaseGifBuffers();
    return false;
  }
  return true;
}

void ImageViewerScreen::drawGifLoading(uint8_t percent) {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  int16_t centerX = _display.width() / 2;
  int16_t centerY = _display.height() / 2;
  tft.fillRect(centerX - 130, centerY - 30, 260, 62, TFT_BLACK);
  UiText::draw(tft, F("Converting animation"), centerX, centerY - 12, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  UiText::draw(tft, String(percent) + F("%"), centerX, centerY + 14,
               TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
  drawCloseIcon();
}

void ImageViewerScreen::drawGifPreloading(uint8_t percent) {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  int16_t centerX = _display.width() / 2;
  int16_t centerY = _display.height() / 2;
  tft.fillRect(centerX - 130, centerY - 30, 260, 62, TFT_BLACK);
  UiText::draw(tft, F("Loading"), centerX, centerY - 12, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  UiText::draw(tft, String(percent) + F("%"), centerX, centerY + 14,
               TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
  drawCloseIcon();
}

void ImageViewerScreen::scaleGifFrame() {
  if (!_frameBuffer || !_gif.getFrameBuf() || _sourceWidth == 0 || _sourceHeight == 0) return;
  uint8_t *buffer = _gif.getFrameBuf();
  const uint16_t *source = reinterpret_cast<const uint16_t *>(buffer + _gifCanvasPixels);
  memset(_frameBuffer, 0, (size_t)_display.width() * _display.height() * sizeof(uint16_t));
  for (uint32_t dy = 0; dy < _drawHeight; dy++) {
    uint32_t sourceY = _drawHeight > 1
      ? (uint32_t)(((uint64_t)dy * (_sourceHeight - 1)) / (_drawHeight - 1)) : 0;
    uint16_t *destination = _frameBuffer +
      (size_t)(_top + dy) * _display.width() + _left;
    const uint16_t *sourceRow = source + (size_t)sourceY * _sourceWidth;
    for (uint32_t dx = 0; dx < _drawWidth; dx++) {
      uint32_t sourceX = _drawWidth > 1
        ? (uint32_t)(((uint64_t)dx * (_sourceWidth - 1)) / (_drawWidth - 1)) : 0;
      destination[dx] = sourceRow[sourceX];
    }
  }
}

bool ImageViewerScreen::readGifCacheFrame(uint32_t frameIndex,
                                          uint16_t *destination,
                                          uint32_t &delay) {
  if (!_gifCacheReadFile || !destination || frameIndex >= _gifFrameCount) return false;
  uint64_t offset = AnimationCacheHeaderSize +
    (uint64_t)frameIndex * (AnimationFrameHeaderSize + CachePixelBytes);
  if (offset > 0xffffffffULL || !_gifCacheReadFile.seek((uint32_t)offset)) return false;
  uint8_t delayBytes[AnimationFrameHeaderSize];
  if (_gifCacheReadFile.read(delayBytes, sizeof(delayBytes)) != sizeof(delayBytes)) return false;
  delay = readLe32(delayBytes);
  uint32_t readOffset = 0;
  while (readOffset < CachePixelBytes) {
    uint32_t remaining = CachePixelBytes - readOffset;
    uint16_t amount = remaining > CacheChunkBytes ? CacheChunkBytes : (uint16_t)remaining;
    if (_gifCacheReadFile.read(_cacheChunk, amount) != amount) return false;
    if (!cacheNeedsRotation()) {
      memcpy((uint8_t *)destination + readOffset, _cacheChunk, amount);
    } else {
      uint32_t firstPixel = readOffset / 2;
      uint32_t pixelCount = amount / 2;
      for (uint32_t i = 0; i < pixelCount; i++) {
        uint32_t cachePixel = firstPixel + i;
        uint32_t cacheX = cachePixel % CacheWidth;
        uint32_t cacheY = cachePixel / CacheWidth;
        uint32_t screenX = cacheY;
        uint32_t screenY = CacheWidth - 1 - cacheX;
        uint16_t color;
        memcpy(&color, _cacheChunk + i * 2, sizeof(color));
        destination[screenY * _display.width() + screenX] = color;
      }
    }
    readOffset += amount;
  }
  return true;
}

void ImageViewerScreen::copyGifCacheChunk(uint16_t *destination,
                                           uint32_t pixelOffset,
                                           const uint8_t *chunk,
                                           uint16_t amount) {
  if (!cacheNeedsRotation()) {
    memcpy((uint8_t *)destination + pixelOffset, chunk, amount);
    return;
  }
  uint32_t firstPixel = pixelOffset / 2;
  uint32_t pixelCount = amount / 2;
  for (uint32_t i = 0; i < pixelCount; i++) {
    uint32_t cachePixel = firstPixel + i;
    uint32_t cacheX = cachePixel % CacheWidth;
    uint32_t cacheY = cachePixel / CacheWidth;
    uint32_t screenX = cacheY;
    uint32_t screenY = CacheWidth - 1 - cacheX;
    uint16_t color;
    memcpy(&color, chunk + i * 2, sizeof(color));
    destination[screenY * _display.width() + screenX] = color;
  }
}

bool ImageViewerScreen::readGifCacheFrameChunk(uint32_t frameIndex,
                                                uint32_t pixelOffset,
                                                uint16_t *destination,
                                                uint32_t &delay) {
  if (!_gifCacheReadFile || !destination || frameIndex >= _gifFrameCount ||
      pixelOffset >= CachePixelBytes) return false;
  uint64_t recordOffset = AnimationCacheHeaderSize +
    (uint64_t)frameIndex * gifFrameRecordBytes();
  if (pixelOffset == 0) {
    if (recordOffset > 0xffffffffULL || !_gifCacheReadFile.seek((uint32_t)recordOffset)) return false;
    uint8_t delayBytes[AnimationFrameHeaderSize];
    if (_gifCacheReadFile.read(delayBytes, sizeof(delayBytes)) != sizeof(delayBytes)) return false;
    delay = readLe32(delayBytes);
  }
  uint64_t pixelFileOffset = recordOffset + AnimationFrameHeaderSize + pixelOffset;
  uint32_t remaining = CachePixelBytes - pixelOffset;
  uint16_t amount = remaining > CacheChunkBytes ? CacheChunkBytes : (uint16_t)remaining;
  if (pixelFileOffset > 0xffffffffULL || !_gifCacheReadFile.seek((uint32_t)pixelFileOffset) ||
      _gifCacheReadFile.read(_cacheChunk, amount) != amount) return false;
  copyGifCacheChunk(destination, pixelOffset, _cacheChunk, amount);
  return true;
}

bool ImageViewerScreen::startGifLoader() {
  if (_gifLoaderTask || _gifResidentCapacity == 0) return false;
  _gifLoadRequests = xQueueCreate(_gifResidentCapacity, sizeof(GifLoadRequest));
  _gifLoadResults = xQueueCreate(_gifResidentCapacity + 1, sizeof(GifLoadResult));
  _gifLoaderStopped = xSemaphoreCreateBinary();
  if (!_gifLoadRequests || !_gifLoadResults || !_gifLoaderStopped) {
    stopGifLoader();
    return false;
  }
  _gifLoaderStop = false;
  _gifLoaderError = false;
#if defined(CONFIG_FREERTOS_UNICORE) && CONFIG_FREERTOS_UNICORE
  const BaseType_t loaderCore = 0;
#else
  const BaseType_t loaderCore = ARDUINO_RUNNING_CORE == 0 ? 1 : 0;
#endif
  if (xTaskCreatePinnedToCore(gifLoaderTask, "gifLoader", 4096, this,
                              tskIDLE_PRIORITY + 1, &_gifLoaderTask,
                              loaderCore) != pdPASS) {
    stopGifLoader();
    return false;
  }
  Serial.print(F("GIF loader core: "));
  Serial.println(loaderCore);
  return true;
}

void ImageViewerScreen::stopGifLoader() {
  if (_gifLoaderTask) {
    _gifLoaderStop = true;
    // The worker checks this at least every queue timeout and releases its
    // file before acknowledging. Never free a PSRAM slot before that point.
    while (xSemaphoreTake(_gifLoaderStopped, pdMS_TO_TICKS(25)) != pdTRUE) {
      vTaskDelay(1);
    }
    _gifLoaderTask = nullptr;
  }
  if (_gifLoadRequests) vQueueDelete(_gifLoadRequests);
  if (_gifLoadResults) vQueueDelete(_gifLoadResults);
  if (_gifLoaderStopped) vSemaphoreDelete(_gifLoaderStopped);
  _gifLoadRequests = nullptr;
  _gifLoadResults = nullptr;
  _gifLoaderStopped = nullptr;
  _gifLoaderStop = false;
}

bool ImageViewerScreen::enqueueGifLoad(uint32_t sequence) {
  if (!_gifLoadRequests || !_gifResidentStates || !canPlayGifSequence(sequence)) return false;
  uint32_t slot = sequence % _gifResidentCapacity;
  if (_gifResidentStates[slot] != (uint8_t)GifSlotState::Free) return false;
  GifLoadRequest request = { slot, sequence };
  _gifResidentStates[slot] = (uint8_t)GifSlotState::Loading;
  _gifResidentSequences[slot] = sequence;
  if (xQueueSend(_gifLoadRequests, &request, 0) != pdTRUE) {
    _gifResidentStates[slot] = (uint8_t)GifSlotState::Free;
    _gifResidentSequences[slot] = 0xffffffffUL;
    return false;
  }
  return true;
}

bool ImageViewerScreen::collectGifLoadResults() {
  if (!_gifLoadResults) return false;
  GifLoadResult result;
  while (xQueueReceive(_gifLoadResults, &result, 0) == pdTRUE) {
    if (!result.success) {
      _gifLoaderError = true;
      _error = F("GIF background cache read failed");
      return false;
    }
    if (result.slot >= _gifResidentCapacity || !_gifResidentStates) continue;
    if (_gifResidentStates[result.slot] == (uint8_t)GifSlotState::Loading &&
        _gifResidentSequences[result.slot] == result.sequence) {
      _gifResidentDelays[result.slot] = result.delay;
      _gifResidentStates[result.slot] = (uint8_t)GifSlotState::Ready;
      _gifLoadedFrames++;
    }
  }
  return !_gifLoaderError;
}

bool ImageViewerScreen::allGifPreloadSlotsReady() const {
  if (!_gifResidentStates) return false;
  for (uint32_t i = 0; i < _gifResidentCapacity; i++) {
    if (_gifResidentStates[i] != (uint8_t)GifSlotState::Ready) return false;
  }
  return true;
}

bool ImageViewerScreen::loadGifFrameOnWorker(StorageFile &cache,
                                              uint8_t *chunk,
                                              const GifLoadRequest &request,
                                              GifLoadResult &result) {
  result = { request.slot, request.sequence, 0, false };
  uint16_t *destination = gifResidentFrame(request.slot);
  uint32_t frameIndex = gifSourceFrameForSequence(request.sequence);
  if (!destination || frameIndex >= _gifFrameCount) return false;
  uint64_t recordOffset = AnimationCacheHeaderSize +
    (uint64_t)frameIndex * gifFrameRecordBytes();
  if (recordOffset > 0xffffffffULL || !SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
  bool ok = cache.seek((uint32_t)recordOffset);
  uint8_t delayBytes[AnimationFrameHeaderSize];
  if (ok) ok = cache.read(delayBytes, sizeof(delayBytes)) == sizeof(delayBytes);
  SpiBusLock::unlock();
  if (!ok) return false;
  result.delay = readLe32(delayBytes);

  memset(destination, 0, (size_t)_display.width() * _display.height() * sizeof(uint16_t));
  uint32_t startedAt = millis();
  uint8_t packed[3];
  uint8_t packedCount = 0;
  uint32_t pixel = 0;
  for (uint32_t offset = 0; offset < gifRgb444Bytes(); ) {
    if (_gifLoaderStop) return false;
    uint32_t remaining = gifRgb444Bytes() - offset;
    // Keep shared SPI ownership short enough for a press and its release to
    // be sampled between loader reads. The expansion work happens unlocked.
    uint16_t amount = remaining > GifLoaderReadChunkBytes ? GifLoaderReadChunkBytes : (uint16_t)remaining;
    // The cursor is positioned immediately after the delay header. Reading
    // sequentially avoids 75 FAT seeks per 480x320 frame.
    if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
    ok = cache.read(chunk, amount) == amount;
    SpiBusLock::unlock();
    if (!ok) return false;
    for (uint16_t i = 0; i < amount; i++) {
      packed[packedCount++] = chunk[i];
      if (packedCount != 3) continue;
      uint16_t colors[2] = {
        (uint16_t)((packed[0] << 4) | (packed[1] >> 4)),
        (uint16_t)(((packed[1] & 0x0f) << 8) | packed[2])
      };
      for (uint8_t which = 0; which < 2 && pixel < (uint32_t)_drawWidth * _drawHeight; which++, pixel++) {
        uint16_t rgb444 = colors[which];
        uint16_t rgb565 = (uint16_t)(((rgb444 & 0x0f00) << 4) |
                                     ((rgb444 & 0x00f0) << 3) |
                                     ((rgb444 & 0x000f) << 1));
        uint16_t x = (uint16_t)(pixel % _drawWidth);
        uint16_t y = (uint16_t)(pixel / _drawWidth);
        destination[(size_t)(_top + y) * _display.width() + _left + x] = rgb565;
      }
      packedCount = 0;
    }
    offset += amount;
    // Give the waiting touch task a scheduling point before requesting the
    // shared bus again on the other core.
    taskYIELD();
  }
  if (packedCount == 2 && pixel < (uint32_t)_drawWidth * _drawHeight) {
    uint16_t rgb444 = (uint16_t)((packed[0] << 4) | (packed[1] >> 4));
    uint16_t rgb565 = (uint16_t)(((rgb444 & 0x0f00) << 4) |
                                 ((rgb444 & 0x00f0) << 3) |
                                 ((rgb444 & 0x000f) << 1));
    uint16_t x = (uint16_t)(pixel % _drawWidth);
    uint16_t y = (uint16_t)(pixel / _drawWidth);
    destination[(size_t)(_top + y) * _display.width() + _left + x] = rgb565;
  }
  (void)startedAt;
  result.success = true;
  return true;
}

void ImageViewerScreen::gifLoaderTask(void *context) {
  ImageViewerScreen *viewer = static_cast<ImageViewerScreen *>(context);
  uint8_t *chunk = (uint8_t *)heap_caps_malloc(CacheChunkBytes,
                                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  StorageFile cache;
  bool opened = chunk != nullptr && SpiBusLock::lock(pdMS_TO_TICKS(100));
  if (opened) {
    cache = viewer->_secretStorage.open(viewer->_cachePath, FILE_READ, viewer->_secretAccess);
    SpiBusLock::unlock();
    opened = (bool)cache;
  }
  while (!viewer->_gifLoaderStop) {
    GifLoadRequest request;
    if (xQueueReceive(viewer->_gifLoadRequests, &request, pdMS_TO_TICKS(20)) != pdTRUE) continue;
    GifLoadResult result = { request.slot, request.sequence, 0, false };
    bool ok = opened && viewer->loadGifFrameOnWorker(cache, chunk, request, result);
    result.success = ok;
    xQueueSend(viewer->_gifLoadResults, &result, portMAX_DELAY);
  }
  if (cache) {
    if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
      cache.close();
      SpiBusLock::unlock();
    }
  }
  if (chunk) heap_caps_free(chunk);
  xSemaphoreGive(viewer->_gifLoaderStopped);
  vTaskDelete(nullptr);
}

uint16_t *ImageViewerScreen::gifResidentFrame(uint32_t slot) const {
  if (!_gifResidentFrames || slot >= _gifResidentCapacity) return nullptr;
  return _gifResidentFrames + (size_t)slot * (CachePixelBytes / sizeof(uint16_t));
}

uint32_t ImageViewerScreen::gifRgb444Bytes() const {
  return ((uint32_t)_drawWidth * _drawHeight * 3UL + 1UL) / 2UL;
}

uint32_t ImageViewerScreen::gifFrameRecordBytes() const {
  return AnimationFrameHeaderSize + gifRgb444Bytes();
}

void ImageViewerScreen::prepareGifDrawRect() {
  uint32_t limitWidth = _display.width() < GifPhotoMaxWidth ? _display.width() : GifPhotoMaxWidth;
  uint32_t limitHeight = _display.height() < GifPhotoMaxHeight ? _display.height() : GifPhotoMaxHeight;
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

void ImageViewerScreen::presentGifFullFrame(const uint16_t *frame) {
  if (!frame || !_display.isReady() || _drawWidth == 0 || _drawHeight == 0) return;
  // The display is cleared before playback begins. Frames only need to update
  // the centered photo rectangle, which leaves the close control's black
  // corner untouched even when the animation itself has a black background.
  _gifPresentFrame = frame;
  _gifPresentY = 0;
  _gifPresentActive = true;
}

void ImageViewerScreen::presentGifFrame(const uint16_t *frame,
                                        const uint16_t *previous,
                                        bool forceFull) {
  (void)previous;
  (void)forceFull;
  presentGifFullFrame(frame);
}

bool ImageViewerScreen::tickGifPresentation() {
  if (!_gifPresentActive) return true;
  if (!_gifPresentFrame || !_display.isReady() || _gifPresentY >= _drawHeight) {
    _gifPresentFrame = nullptr;
    _gifPresentActive = false;
    return false;
  }

  // One small transfer per Arduino loop gives updateTouch() a chance to
  // observe both press and release edges between animation bands.
  const uint16_t bandHeight = GifPresentBandRows;
  uint16_t rows = _drawHeight - _gifPresentY;
  if (rows > bandHeight) rows = bandHeight;
  const uint16_t width = _display.width();
  // Frame buffers retain the full-screen stride even though only the photo
  // rectangle changes. Stage the band as tightly packed rows before giving
  // it to TFT_eSPI; passing the first row directly corrupts every following
  // row when the photo is narrower than the display.
  for (uint16_t row = 0; row < rows; row++) {
    const uint16_t *source = _gifPresentFrame +
      (size_t)(_top + _gifPresentY + row) * width + _left;
    memcpy(_gifPresentBuffer + (size_t)row * _drawWidth, source,
           (size_t)_drawWidth * sizeof(uint16_t));
  }
  _display.tft().pushImage(_left, _top + _gifPresentY, _drawWidth, rows,
                            _gifPresentBuffer);
  _gifPresentY += rows;
  if (_gifPresentY == _drawHeight) {
    _gifPresentFrame = nullptr;
    _gifPresentActive = false;
  }
  return true;
}

uint32_t ImageViewerScreen::gifSourceFrameForSequence(uint32_t sequence) const {
  return _gifFrameCount ? sequence % _gifFrameCount : 0;
}

bool ImageViewerScreen::canPlayGifSequence(uint32_t sequence) const {
  if (_gifFrameCount == 0) return false;
  if (_gifLoopCount == 0) return true;
  if (_gifLoopCount < 0) return sequence < _gifFrameCount;
  uint64_t totalFrames = (uint64_t)_gifFrameCount * ((uint64_t)_gifLoopCount + 1ULL);
  return (uint64_t)sequence < totalFrames;
}

bool ImageViewerScreen::beginGifRingLoad(uint32_t sequence) {
  if (!_gifResidentFrames || !_gifResidentDelays || !_gifResidentSequences ||
      _gifResidentCapacity == 0 || !canPlayGifSequence(sequence)) return false;
  uint32_t slot = _gifFullResident ? gifSourceFrameForSequence(sequence) :
    sequence % _gifResidentCapacity;
  _gifPreloadSlot = slot;
  _gifPreloadSequence = sequence;
  _gifPreloadOffset = 0;
  _gifPreloadActive = true;
  return true;
}

bool ImageViewerScreen::tickGifRingLoad() {
  if (!_gifPreloadActive) return true;
  uint16_t *destination = gifResidentFrame(_gifPreloadSlot);
  if (!destination) return false;
  uint8_t chunks = GifPreloadChunksPerTick;
  while (chunks-- && _gifPreloadOffset < CachePixelBytes) {
    if (!readGifCacheFrameChunk(gifSourceFrameForSequence(_gifPreloadSequence),
                                _gifPreloadOffset, destination,
                                _gifResidentDelays[_gifPreloadSlot])) {
      return false;
    }
    uint32_t remaining = CachePixelBytes - _gifPreloadOffset;
    _gifPreloadOffset += remaining > CacheChunkBytes ? CacheChunkBytes : remaining;
  }
  if (_gifPreloadOffset == CachePixelBytes) {
    _gifResidentSequences[_gifPreloadSlot] = _gifPreloadSequence;
    _gifPreloadActive = false;
  }
  return true;
}

bool ImageViewerScreen::startNextGifRingLoad() {
  if (_gifFullResident || !canPlayGifSequence(_gifNextLoadSequence)) return true;
  if (_gifLoaderTask) {
    uint32_t slot = _gifNextLoadSequence % _gifResidentCapacity;
    if (_gifNextLoadSequence < _gifResidentCapacity) {
      if (!_gifResidentStates || _gifResidentStates[slot] != (uint8_t)GifSlotState::Free) return true;
      return enqueueGifLoad(_gifNextLoadSequence++);
    }
    uint32_t previousSequence = _gifNextLoadSequence - _gifResidentCapacity;
    if (!_gifResidentStates || _gifResidentStates[slot] != (uint8_t)GifSlotState::Free ||
        _gifResidentSequences[slot] != previousSequence) return true;
    return enqueueGifLoad(_gifNextLoadSequence++);
  }
  if (_gifPreloadActive) return true;
  uint32_t slot = _gifNextLoadSequence % _gifResidentCapacity;
  if (_gifNextLoadSequence < _gifResidentCapacity) {
    if (_gifResidentSequences[slot] != 0xffffffffUL) return true;
    if (!beginGifRingLoad(_gifNextLoadSequence)) return false;
    _gifNextLoadSequence++;
    return true;
  }
  uint32_t previousSequence = _gifNextLoadSequence - _gifResidentCapacity;
  if (_gifResidentSequences[slot] != previousSequence || previousSequence >= _gifPlaybackSequence) return true;
  if (!beginGifRingLoad(_gifNextLoadSequence)) return false;
  _gifNextLoadSequence++;
  return true;
}

bool ImageViewerScreen::startGifPreload() {
  if (!psramFound() || _gifFrameCount == 0 || !_gifCacheReadFile) return false;
  uint32_t freePsram = ESP.getFreePsram();
  uint32_t maxBlock = ESP.getMaxAllocPsram();
  uint32_t usable = freePsram > GifPsramReserveBytes ? freePsram - GifPsramReserveBytes : 0;
  if (maxBlock < usable) usable = maxBlock;
  uint32_t capacity = usable / CachePixelBytes;
  if (capacity == 0) return false;
  if (capacity > _gifFrameCount) capacity = _gifFrameCount;
  uint32_t minimumCapacity = _gifFrameCount < 3 ? _gifFrameCount : 3;
  if (capacity < minimumCapacity) return false;

  while (capacity >= minimumCapacity) {
    uint64_t bytes = (uint64_t)capacity * CachePixelBytes;
    if (bytes > 0xffffffffULL) break;
    _gifResidentFrames = (uint16_t *)ps_malloc((size_t)bytes);
    if (_gifResidentFrames) break;
    capacity--;
  }
  if (!_gifResidentFrames || capacity < minimumCapacity) {
    _gifResidentFrames = nullptr;
    return false;
  }
  _gifResidentDelays = (uint32_t *)malloc((size_t)capacity * sizeof(uint32_t));
  _gifResidentSequences = (uint32_t *)malloc((size_t)capacity * sizeof(uint32_t));
  _gifResidentStates = (uint8_t *)malloc(capacity);
  if (!_gifResidentDelays || !_gifResidentSequences || !_gifResidentStates) {
    releaseGifBuffers();
    return false;
  }
  for (uint32_t i = 0; i < capacity; i++) {
    _gifResidentSequences[i] = 0xffffffffUL;
    _gifResidentStates[i] = (uint8_t)GifSlotState::Free;
  }
  _gifResidentCapacity = capacity;
  _gifFullResident = capacity == _gifFrameCount;
  _gifPreloadTarget = _gifFullResident ? capacity :
    (capacity < GifInitialPreloadFrames ? capacity : GifInitialPreloadFrames);
  _gifNextLoadSequence = _gifPreloadTarget;
  Serial.print(F("GIF PSRAM preload: "));
  Serial.print(capacity);
  Serial.print(F(" frame(s), "));
  Serial.println(_gifFullResident ? F("full cycle") : F("ring buffer"));
  _gifState = GifState::Preloading;
  // Cache hits enter directly from FilesScreen, so clear the full display
  // before drawing the small preload overlay.
  draw();
  drawGifPreloading(0);
  if (EnableBackgroundGifLoader) {
    if (!startGifLoader()) {
      releaseGifBuffers();
      return false;
    }
    if (_gifCacheReadFile) _gifCacheReadFile.close();
    _lastProgress = 0;
    uint32_t initialFrames = _gifFullResident ? capacity : _gifPreloadTarget;
    for (uint32_t sequence = 0; sequence < initialFrames; sequence++) {
      if (!enqueueGifLoad(sequence)) {
        releaseGifBuffers();
        return false;
      }
    }
  }
  if (!EnableBackgroundGifLoader && !beginGifRingLoad(0)) {
    releaseGifBuffers();
    return false;
  }
  return true;
}

bool ImageViewerScreen::tickGifPreload() {
  if (_gifLoaderTask) {
    if (!collectGifLoadResults()) return false;
    uint32_t ready = 0;
    for (uint32_t i = 0; i < _gifResidentCapacity; i++) {
      if (_gifResidentStates[i] == (uint8_t)GifSlotState::Ready) ready++;
    }
    if (ready != _lastProgress) {
      _lastProgress = (uint8_t)ready;
      uint32_t progress = _gifPreloadTarget == 0 ? 100 : (ready * 100UL) / _gifPreloadTarget;
      drawGifPreloading((uint8_t)(progress > 100 ? 100 : progress));
    }
    return ready >= _gifPreloadTarget ? beginGifResidentPlayback() : true;
  }
  if (!tickGifRingLoad()) return false;
  if (_gifPreloadActive) return true;
  uint32_t completed = _gifPreloadSequence + 1;
  uint32_t progress = _gifResidentCapacity == 0 ? 100 : (completed * 100UL) / _gifResidentCapacity;
  drawGifPreloading((uint8_t)(progress > 100 ? 100 : progress));
  if (completed < _gifResidentCapacity) return beginGifRingLoad(completed);
  return beginGifResidentPlayback();
}

bool ImageViewerScreen::beginGifResidentPlayback() {
  if (!_gifResidentFrames || !_gifResidentSequences || _gifResidentSequences[0] != 0) return false;
  _gifPlaybackSequence = 0;
  _gifPlaybackFrame = 0;
  _gifPlaybackDelay = _gifResidentDelays[0];
  if (_gifResidentStates) _gifResidentStates[0] = (uint8_t)GifSlotState::Displaying;
  draw();
  drawCloseIcon();
  presentGifFrame(gifResidentFrame(0), nullptr, true);
  _gifNextFrameAt = millis() + (_gifPlaybackDelay ? _gifPlaybackDelay : GifMinimumDelayMs);
  _gifCyclesCompleted = 0;
  _gifState = GifState::Playing;
  if (_gifFullResident && _gifCacheReadFile) _gifCacheReadFile.close();
  return true;
}

bool ImageViewerScreen::startGifPlayback() {
  if (!_gifCacheReadFile || _gifFrameCount == 0) return false;
  if (!cacheNeedsRotation() &&
      (_display.width() != CacheWidth || _display.height() != CacheHeight)) {
    _error = F("GIF cache does not support this display size");
    return false;
  }
  if (!psramFound()) {
    _error = F("GIF viewer requires PSRAM");
    return false;
  }
  // A three-slot ring is the minimum safe configuration: displayed, next,
  // and worker-owned. Do not fall back to synchronous two-frame SD reads;
  // they are the source of visible playback freezes.
  if (startGifPreload()) return true;
  _error = F("GIF playback needs 3 PSRAM frames");
  return false;
}

void ImageViewerScreen::beginGifTwoFrameLoad(uint32_t frameIndex) {
  _gifBufferedFrame = frameIndex;
  _gifBufferedOffset = 0;
  _gifBufferedLoading = true;
  _gifNextLoaded = false;
  _gifNextDelay = 0;
}

bool ImageViewerScreen::tickGifTwoFrameLoad() {
  if (!_gifBufferedLoading) return true;
  uint16_t *destination = _gifActiveIsA ? _gifPlaybackB : _gifPlaybackA;
  if (!destination || _gifBufferedFrame >= _gifFrameCount) return false;
  uint8_t chunks = GifPreloadChunksPerTick;
  while (chunks-- && _gifBufferedOffset < CachePixelBytes) {
    if (!readGifCacheFrameChunk(_gifBufferedFrame, _gifBufferedOffset,
                                destination, _gifNextDelay)) {
      return false;
    }
    uint32_t remaining = CachePixelBytes - _gifBufferedOffset;
    _gifBufferedOffset += remaining > CacheChunkBytes ? CacheChunkBytes : remaining;
  }
  if (_gifBufferedOffset == CachePixelBytes) {
    _gifBufferedLoading = false;
    _gifNextLoaded = true;
  }
  return true;
}

bool ImageViewerScreen::tickGifPlayback(uint32_t nowMs) {
  if (_gifPresentActive) return tickGifPresentation();
  if (_gifResidentFrames) {
    if (_gifLoaderTask) {
      if (!collectGifLoadResults() || !startNextGifRingLoad()) return false;
    } else if (!tickGifRingLoad() || !startNextGifRingLoad()) {
      return false;
    }
    if ((int32_t)(nowMs - _gifNextFrameAt) < 0) return true;
    uint32_t nextSequence = _gifPlaybackSequence + 1;
    if (!canPlayGifSequence(nextSequence)) {
      _gifState = GifState::None;
      return true;
    }
    uint32_t slot = _gifFullResident ? gifSourceFrameForSequence(nextSequence) :
      nextSequence % _gifResidentCapacity;
    bool nextFrameReady = _gifLoaderTask
      ? (_gifResidentStates &&
         _gifResidentStates[slot] == (uint8_t)GifSlotState::Ready &&
         _gifResidentSequences[slot] == nextSequence)
      : _gifResidentSequences[slot] == nextSequence;
    if (!_gifFullResident && !nextFrameReady) {
      if (!_gifWaitingForFrame) {
        _gifWaitingForFrame = true;
        _gifUnderruns++;
        Serial.print(F("GIF ring underrun: "));
        Serial.println(_gifUnderruns);
      }
      // Keep the timeline moving. When the worker catches up the next tick
      // can present the queued frame instead of stretching every following
      // GIF delay by the underrun duration.
      _gifDroppedFrames++;
      _gifNextFrameAt += _gifPlaybackDelay ? _gifPlaybackDelay : GifMinimumDelayMs;
      Serial.print(F("GIF dropped frames: "));
      Serial.println(_gifDroppedFrames);
      return true;
    }
    uint16_t *next = gifResidentFrame(slot);
    if (!next) return false;
    uint32_t previousSequence = _gifPlaybackSequence;
    uint32_t previousSlot = _gifFullResident ? gifSourceFrameForSequence(previousSequence) :
      previousSequence % _gifResidentCapacity;
    _gifPlaybackSequence = nextSequence;
    _gifPlaybackFrame = gifSourceFrameForSequence(nextSequence);
    _gifPlaybackDelay = _gifResidentDelays[slot];
    _gifWaitingForFrame = false;
    if (_gifResidentStates) {
      _gifResidentStates[slot] = (uint8_t)GifSlotState::Displaying;
      if (!_gifFullResident) _gifResidentStates[previousSlot] = (uint8_t)GifSlotState::Free;
    }
    presentGifFrame(next, gifResidentFrame(previousSlot), false);
    _gifNextFrameAt += _gifPlaybackDelay ? _gifPlaybackDelay : GifMinimumDelayMs;
    return startNextGifRingLoad();
  }
  if (!tickGifTwoFrameLoad()) return false;
  if ((int32_t)(nowMs - _gifNextFrameAt) < 0) return true;
  if (_gifFrameCount == 1) {
    if (_gifLoopCount < 0) {
      _gifState = GifState::None;
      return true;
    }
    if (_gifLoopCount != 0 && _gifCyclesCompleted >= (uint32_t)_gifLoopCount) {
      _gifState = GifState::None;
      return true;
    }
    _gifCyclesCompleted++;
    presentGifFullFrame(_gifPlaybackA);
    _gifNextFrameAt = nowMs + (_gifPlaybackDelay ? _gifPlaybackDelay : 1);
    return true;
  }
  // Keep the current image visible until the next frame has been read in
  // bounded chunks. This avoids a 307 KB blocking SD read in one screen tick.
  if (!_gifNextLoaded) return true;
  uint16_t *active = _gifActiveIsA ? _gifPlaybackB : _gifPlaybackA;
  _gifActiveIsA = !_gifActiveIsA;
  uint32_t displayedFrame = _gifBufferedFrame;
  _gifPlaybackFrame = displayedFrame;
  _gifPlaybackDelay = _gifNextDelay;
  presentGifFullFrame(active);

  if (displayedFrame + 1 < _gifFrameCount) {
    beginGifTwoFrameLoad(displayedFrame + 1);
  } else {
    bool repeat = _gifLoopCount == 0 ||
      (_gifLoopCount > 0 && _gifCyclesCompleted < (uint32_t)_gifLoopCount);
    if (repeat) {
      _gifCyclesCompleted++;
      beginGifTwoFrameLoad(0);
    } else {
      _gifNextLoaded = false;
      _gifState = GifState::None;
      return true;
    }
  }
  _gifNextFrameAt = nowMs + (_gifPlaybackDelay ? _gifPlaybackDelay : 1);
  return true;
}

void ImageViewerScreen::releaseGifBuffers() {
  stopGifLoader();
  if (_gifPlaybackA) free(_gifPlaybackA);
  if (_gifPlaybackB) free(_gifPlaybackB);
  if (_gifResidentFrames) free(_gifResidentFrames);
  if (_gifResidentDelays) free(_gifResidentDelays);
  if (_gifResidentSequences) free(_gifResidentSequences);
  if (_gifResidentStates) free(_gifResidentStates);
  _gifPlaybackA = nullptr;
  _gifPlaybackB = nullptr;
  _gifBufferedFrame = 0;
  _gifBufferedOffset = 0;
  _gifBufferedLoading = false;
  _gifPresentFrame = nullptr;
  _gifPresentY = 0;
  _gifPresentActive = false;
  _gifResidentFrames = nullptr;
  _gifResidentDelays = nullptr;
  _gifResidentSequences = nullptr;
  _gifResidentStates = nullptr;
  _gifResidentCapacity = 0;
  _gifPreloadTarget = 0;
  _gifPreloadSlot = 0;
  _gifPreloadSequence = 0;
  _gifPreloadOffset = 0;
  _gifNextLoadSequence = 0;
  _gifPlaybackSequence = 0;
  _gifFullResident = false;
  _gifPreloadActive = false;
  _gifLoaderError = false;
  _gifLoadedFrames = 0;
  _gifUnderruns = 0;
  _gifDroppedFrames = 0;
  _gifWaitingForFrame = false;
  _gifNextLoaded = false;
}

void ImageViewerScreen::releaseGifDecoder() {
  if (_gifDecoderOpen) {
    _gif.close();
    _gifDecoderOpen = false;
  }
  _decoderOwner = nullptr;
  if (_gif.getFrameBuf()) _gif.freeFrameBuf(gifFree);
}

bool ImageViewerScreen::isGifSource() {
  uint8_t signature[6] = {};
  if (!_file || !_file.seek(0) || _file.read(signature, sizeof(signature)) != sizeof(signature)) return false;
  return FileFormats::fromSignature(signature, sizeof(signature)) == FileFormats::Format::Gif;
}

bool ImageViewerScreen::isWebpSource() {
  uint8_t signature[12] = {};
  if (!_file || !_file.seek(0) || _file.read(signature, sizeof(signature)) != sizeof(signature)) {
    return false;
  }
  return FileFormats::fromSignature(signature, sizeof(signature)) == FileFormats::Format::Webp;
}

bool ImageViewerScreen::prepareWebpSource() {
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
  if (!_file.seek(0)) {
    _error = F("WebP source seek failed");
    releaseWebpDecoder();
    return false;
  }
  uint32_t offset = 0;
  while (offset < _webpSourceSize) {
    uint16_t amount = (uint16_t)((_webpSourceSize - offset) > CacheChunkBytes
                                   ? CacheChunkBytes : (_webpSourceSize - offset));
    if (_file.read(_cacheChunk, amount) != amount) {
      _error = F("WebP source read failed");
      releaseWebpDecoder();
      return false;
    }
    memcpy(_webpSource + offset, _cacheChunk, amount);
    offset += amount;
  }
  return true;
}

bool ImageViewerScreen::inspectWebpSource() {
  if (!prepareWebpSource()) return false;
  WebPBitstreamFeatures features;
  if (WebPGetFeatures(_webpSource, _webpSourceSize, &features) != VP8_STATUS_OK ||
      features.width <= 0 || features.height <= 0) {
    _error = F("Invalid WebP header");
    releaseWebpDecoder();
    return false;
  }
  _sourceWidth = (uint32_t)features.width;
  _sourceHeight = (uint32_t)features.height;
  _webpAnimated = features.has_animation != 0;
  if (!_webpAnimated) return true;

  WebPData data = { _webpSource, _webpSourceSize };
  WebPAnimDecoderOptions options;
  if (!WebPAnimDecoderOptionsInit(&options)) {
    _error = F("WebP animation setup failed");
    releaseWebpDecoder();
    return false;
  }
  options.color_mode = MODE_RGBA;
  options.use_threads = 0;
  _webpAnimDecoder = WebPAnimDecoderNew(&data, &options);
  if (!_webpAnimDecoder || !WebPAnimDecoderGetInfo(_webpAnimDecoder, &_webpAnimInfo) ||
      _webpAnimInfo.frame_count == 0) {
    _error = F("Invalid WebP animation");
    releaseWebpDecoder();
    return false;
  }
  _sourceWidth = _webpAnimInfo.canvas_width;
  _sourceHeight = _webpAnimInfo.canvas_height;
  _gifFrameCount = _webpAnimInfo.frame_count;
  _gifLoopCount = (int32_t)_webpAnimInfo.loop_count;
  return true;
}

void ImageViewerScreen::scaleWebpRgba(const uint8_t *rgba) {
  if (!_frameBuffer || !rgba || _sourceWidth == 0 || _sourceHeight == 0) return;
  memset(_frameBuffer, 0, (size_t)_display.width() * _display.height() * sizeof(uint16_t));
  for (uint32_t dy = 0; dy < _drawHeight; dy++) {
    uint32_t sourceY = _drawHeight > 1
      ? (uint32_t)(((uint64_t)dy * (_sourceHeight - 1)) / (_drawHeight - 1)) : 0;
    uint16_t *destination = _frameBuffer +
      (size_t)(_top + dy) * _display.width() + _left;
    const uint8_t *sourceRow = rgba + (size_t)sourceY * _sourceWidth * 4;
    for (uint32_t dx = 0; dx < _drawWidth; dx++) {
      uint32_t sourceX = _drawWidth > 1
        ? (uint32_t)(((uint64_t)dx * (_sourceWidth - 1)) / (_drawWidth - 1)) : 0;
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

bool ImageViewerScreen::decodeWebpImage() {
  if (_webpAnimated || !_webpSource || !_frameBuffer) return false;
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

bool ImageViewerScreen::startWebpBuild() {
  if (_format != ImageFormat::Webp || !_webpAnimated || !_webpAnimDecoder || !_frameBuffer) return false;
  if (!cacheNeedsRotation() &&
      (_display.width() != CacheWidth || _display.height() != CacheHeight)) {
    _error = F("WebP cache does not support this display size");
    return false;
  }
  int slash = _cachePath.lastIndexOf('/');
  String directory = slash <= 0 ? F("/") : _cachePath.substring(0, slash);
  if (!_secretStorage.mkdir(directory)) {
    _error = F("Cannot create WebP cache directory");
    return false;
  }
  _secretStorage.remove(_cacheTempPath);
  _cacheFile = _secretStorage.open(_cacheTempPath, FILE_WRITE, _secretAccess);
  if (!_cacheFile || !writeAnimationCacheHeader(_cacheFile, 0, _gifLoopCount)) {
    _error = F("Cannot start WebP cache");
    if (_cacheFile) _cacheFile.close();
    _secretStorage.remove(_cacheTempPath);
    return false;
  }
  _gifBuildFrame = 0;
  _gifBuildWriteOffset = 0;
  _gifBuildWritePending = false;
  _gifBuildLastFrame = false;
  _gifBuildDelay = 0;
  _webpPreviousTimestamp = 0;
  _gifState = GifState::Building;
  drawGifLoading(0);
  return true;
}

bool ImageViewerScreen::buildWebpFrame() {
  if (!_cacheFile || !_webpAnimDecoder || !_frameBuffer) return false;
  if (_gifBuildWritePending) {
    uint32_t recordBytes = gifFrameRecordBytes();
    uint32_t remaining = recordBytes - _gifBuildWriteOffset;
    uint16_t amount = remaining > CacheChunkBytes ? CacheChunkBytes : (uint16_t)remaining;
    if (_gifBuildWriteOffset < AnimationFrameHeaderSize) {
      uint8_t delay[AnimationFrameHeaderSize] = {};
      writeLe32(delay, _gifBuildDelay);
      uint16_t available = AnimationFrameHeaderSize - _gifBuildWriteOffset;
      if (amount > available) amount = available;
      memcpy(_cacheChunk, delay + _gifBuildWriteOffset, amount);
    } else {
      uint32_t payloadOffset = _gifBuildWriteOffset - AnimationFrameHeaderSize;
      for (uint16_t i = 0; i < amount; i++) {
        uint32_t position = payloadOffset + i;
        uint32_t pair = position / 3;
        uint32_t firstPixel = pair * 2;
        uint16_t x0 = (uint16_t)(firstPixel % _drawWidth);
        uint16_t y0 = (uint16_t)(firstPixel / _drawWidth);
        uint16_t first = _frameBuffer[(size_t)(_top + y0) * _display.width() + _left + x0];
        uint16_t rgb444a = (uint16_t)((first & 0xf000) >> 4) |
                           (uint16_t)((first & 0x0780) >> 3) |
                           (uint16_t)((first & 0x001e) >> 1);
        uint16_t rgb444b = 0;
        if (firstPixel + 1 < (uint32_t)_drawWidth * _drawHeight) {
          uint16_t x1 = (uint16_t)((firstPixel + 1) % _drawWidth);
          uint16_t y1 = (uint16_t)((firstPixel + 1) / _drawWidth);
          uint16_t second = _frameBuffer[(size_t)(_top + y1) * _display.width() + _left + x1];
          rgb444b = (uint16_t)((second & 0xf000) >> 4) |
                       (uint16_t)((second & 0x0780) >> 3) |
                       (uint16_t)((second & 0x001e) >> 1);
        }
        switch (position % 3) {
          case 0: _cacheChunk[i] = (uint8_t)(rgb444a >> 4); break;
          case 1: _cacheChunk[i] = (uint8_t)((rgb444a << 4) | (rgb444b >> 8)); break;
          default: _cacheChunk[i] = (uint8_t)rgb444b; break;
        }
      }
    }
    if (_cacheFile.write(_cacheChunk, amount) != amount) {
      _error = F("WebP cache write failed");
      return false;
    }
    _gifBuildWriteOffset += amount;
    if (_gifBuildWriteOffset == recordBytes) {
      _gifBuildWritePending = false;
      _gifBuildFrame++;
      drawGifLoading((uint8_t)((_gifBuildFrame * 100UL) / _gifFrameCount));
      if (_gifBuildLastFrame) return finishWebpBuild();
    }
    return true;
  }
  if (!WebPAnimDecoderHasMoreFrames(_webpAnimDecoder)) return false;
  uint8_t *rgba = nullptr;
  int timestamp = 0;
  if (!WebPAnimDecoderGetNext(_webpAnimDecoder, &rgba, &timestamp) || !rgba) {
    _error = F("WebP animation decode failed");
    return false;
  }
  uint32_t currentTimestamp = timestamp > 0 ? (uint32_t)timestamp : 0;
  uint32_t delay = currentTimestamp >= _webpPreviousTimestamp
    ? currentTimestamp - _webpPreviousTimestamp : 0;
  _webpPreviousTimestamp = currentTimestamp;
  _gifBuildDelay = delay < GifMinimumDelayMs ? GifMinimumDelayMs : delay;
  scaleWebpRgba(rgba);
  _gifBuildLastFrame = (_gifBuildFrame + 1 >= _gifFrameCount);
  _gifBuildWriteOffset = 0;
  _gifBuildWritePending = true;
  return true;
}

bool ImageViewerScreen::finishWebpBuild() {
  if (!_cacheFile.seek(0) || !writeAnimationCacheHeader(_cacheFile, _gifBuildFrame, _gifLoopCount)) {
    _error = F("WebP cache header write failed");
    return false;
  }
  _cacheFile.close();
  if (_secretStorage.exists(_cachePath)) _secretStorage.remove(_cachePath);
  if (!_secretStorage.rename(_cacheTempPath, _cachePath)) {
    _error = F("WebP cache commit failed");
    _secretStorage.remove(_cacheTempPath);
    return false;
  }
  releaseWebpDecoder();
  releaseBuffers();
  _gifCacheReadFile = _secretStorage.open(_cachePath, FILE_READ, _secretAccess);
  if (!_gifCacheReadFile || !startGifPlayback()) {
    _error = F("WebP cache reopen failed");
    if (_gifCacheReadFile) _gifCacheReadFile.close();
    releaseGifBuffers();
    return false;
  }
  return true;
}

void ImageViewerScreen::releaseWebpDecoder() {
  if (_webpAnimDecoder) {
    WebPAnimDecoderDelete(_webpAnimDecoder);
    _webpAnimDecoder = nullptr;
  }
  if (_webpRgba) {
    free(_webpRgba);
    _webpRgba = nullptr;
  }
  if (_webpSource) {
    free(_webpSource);
    _webpSource = nullptr;
  }
  _webpSourceSize = 0;
  _webpAnimated = false;
  memset(&_webpAnimInfo, 0, sizeof(_webpAnimInfo));
}

bool ImageViewerScreen::writeCacheHeader(StorageFile &file) const {
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
  if (!_secretStorage.mkdir(directory)) {
    Serial.print(F("Image cache: cannot create "));
    Serial.println(directory);
    return;
  }
  _secretStorage.remove(_cacheTempPath);
  _cacheFile = _secretStorage.open(_cacheTempPath, FILE_WRITE, _secretAccess);
  if (!_cacheFile || !writeCacheHeader(_cacheFile)) {
    Serial.print(F("Image cache: cannot start "));
    Serial.println(_cacheTempPath);
    if (_cacheFile) _cacheFile.close();
    _secretStorage.remove(_cacheTempPath);
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
    bool removedOld = !_cachePath.length() || !_secretStorage.exists(_cachePath) || _secretStorage.remove(_cachePath);
    committed = removedOld && _secretStorage.rename(_cacheTempPath, _cachePath);
  }
  if (!committed) {
    _secretStorage.remove(_cacheTempPath);
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
  if (_format == ImageFormat::Gif || (_format == ImageFormat::Webp && _webpAnimated)) {
    prepareGifDrawRect();
  } else if ((uint64_t)screenWidth * _sourceHeight <= (uint64_t)screenHeight * _sourceWidth) {
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
  if (_format != ImageFormat::Gif)
    _xCoordinates = (uint32_t *)malloc((size_t)_drawWidth * sizeof(uint32_t));
  if (_format == ImageFormat::Bmp) {
    _row0 = (uint8_t *)malloc(sourceBytes);
    _row1 = (uint8_t *)malloc(sourceBytes);
    if (!_rle) _rawRow = (uint8_t *)malloc(_rowStride);
  }
  if (!_frameBuffer || (_format != ImageFormat::Gif && !_xCoordinates) ||
      (_format == ImageFormat::Bmp && (!_row0 || !_row1 || (!_rle && !_rawRow)))) {
    _error = F("Image needs too much memory");
    releaseBuffers();
    return false;
  }
  memset(_frameBuffer, 0, (size_t)screenWidth * screenHeight * sizeof(uint16_t));
  if (_format != ImageFormat::Gif) {
    for (uint32_t dx = 0; dx < _drawWidth; dx++) {
      _xCoordinates[dx] = _drawWidth > 1
        ? (uint32_t)(((uint64_t)dx * (_sourceWidth - 1) << 16) / (_drawWidth - 1)) : 0;
    }
  } else {
    if ((uint64_t)_sourceWidth * _sourceHeight > 0xffffffffULL) {
      _error = F("GIF canvas is too large");
      releaseBuffers();
      return false;
    }
    _gifCanvasPixels = _sourceWidth * _sourceHeight;
    if (_gifCanvasPixels > (0xffffffffUL - (uint32_t)_sourceWidth * 3UL) / 5UL) {
      _error = F("GIF canvas is too large");
      releaseGifDecoder();
      releaseBuffers();
      return false;
    }
    _gif.setDrawType(GIF_DRAW_COOKED);
    if (_gif.allocFrameBuf(gifAlloc) != GIF_SUCCESS) {
      _error = F("GIF needs too much PSRAM");
      releaseGifDecoder();
      releaseBuffers();
      return false;
    }
    memset(_gif.getFrameBuf(), 0,
           (size_t)_gifCanvasPixels * 5 + (size_t)_sourceWidth * 3);
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
  } else if (_format == ImageFormat::Webp) {
    return decodeWebpImage();
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
  _decoderOwner->_decoderFile = _decoderOwner->_secretStorage.open(name, FILE_READ, _decoderOwner->_secretAccess);
  if (!_decoderOwner->_decoderFile) return nullptr;
  *size = (int32_t)_decoderOwner->_decoderFile.size();
  return &_decoderOwner->_decoderFile;
}

void ImageViewerScreen::pngClose(void *handle) {
  StorageFile *file = static_cast<StorageFile *>(handle);
  if (file) file->close();
}

int32_t ImageViewerScreen::pngRead(PNGFILE *file, uint8_t *buffer, int32_t length) {
  if (!file || !file->fHandle) return 0;
  StorageFile *source = static_cast<StorageFile *>(file->fHandle);
  return source->read(buffer, length);
}

int32_t ImageViewerScreen::pngSeek(PNGFILE *file, int32_t position) {
  if (!file || !file->fHandle || position < 0) return 0;
  StorageFile *source = static_cast<StorageFile *>(file->fHandle);
  return source->seek(position) ? position : 0;
}

void *ImageViewerScreen::jpegOpen(const char *name, int32_t *size) { return pngOpen(name, size); }
void ImageViewerScreen::jpegClose(void *handle) { pngClose(handle); }
int32_t ImageViewerScreen::jpegRead(JPEGFILE *file, uint8_t *buffer, int32_t length) {
  if (!file || !file->fHandle || length <= 0) return 0;
  return static_cast<StorageFile *>(file->fHandle)->read(buffer, length);
}
int32_t ImageViewerScreen::jpegSeek(JPEGFILE *file, int32_t position) {
  if (!file || !file->fHandle || position < 0) return 0;
  return static_cast<StorageFile *>(file->fHandle)->seek(position) ? position : 0;
}

int ImageViewerScreen::pngDraw(PNGDRAW *draw) {
  if (!_decoderOwner || !draw || draw->iWidth <= 0) return 0;
  ImageViewerScreen &viewer = *_decoderOwner;
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

void *ImageViewerScreen::gifOpen(const char *name, int32_t *size) {
  if (!_decoderOwner) return nullptr;
  _decoderOwner->_decoderFile = _decoderOwner->_secretStorage.open(name, FILE_READ, _decoderOwner->_secretAccess);
  if (!_decoderOwner->_decoderFile) return nullptr;
  *size = (int32_t)_decoderOwner->_decoderFile.size();
  return &_decoderOwner->_decoderFile;
}

void ImageViewerScreen::gifClose(void *handle) {
  StorageFile *file = static_cast<StorageFile *>(handle);
  if (file) file->close();
}

int32_t ImageViewerScreen::gifRead(GIFFILE *file, uint8_t *buffer, int32_t length) {
  if (!file || !file->fHandle || length <= 0) return 0;
  StorageFile *source = static_cast<StorageFile *>(file->fHandle);
  int32_t amount = length;
  int32_t remaining = file->iSize - file->iPos;
  if (amount > remaining) amount = remaining;
  if (amount <= 0) return 0;
  int32_t read = source->read(buffer, amount);
  file->iPos += read;
  return read;
}

int32_t ImageViewerScreen::gifSeek(GIFFILE *file, int32_t position) {
  if (!file || !file->fHandle || position < 0) return 0;
  StorageFile *source = static_cast<StorageFile *>(file->fHandle);
  if (!source->seek(position)) return 0;
  file->iPos = position;
  return position;
}

void ImageViewerScreen::gifDraw(GIFDRAW *draw) {
  (void)draw;
}

void *ImageViewerScreen::gifAlloc(uint32_t size) {
  return ps_malloc(size);
}

void ImageViewerScreen::gifFree(void *buffer) {
  free(buffer);
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
