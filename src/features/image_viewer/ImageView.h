#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

// JPEGDEC 1.8.x declares its filesystem overload with an unqualified File.
// ESP32's FS core keeps File in namespace fs, so provide the expected alias
// before including the decoder header.
using File = fs::File;

#ifndef PNG_MAX_BUFFERED_PIXELS
// PNGdec needs two uncompressed input rows. This supports common images up
// to 4096 pixels wide, including 32-bit RGBA PNG files.
#define PNG_MAX_BUFFERED_PIXELS (((4096 * 4 + 1) * 2) + 32)
#endif

#include <JPEGDEC.h>
#include <PNGdec.h>
#include "../../../third_party/libwebp/src/webp/decode.h"

#include "../../core/DisplayController.h"
#include "../../services/storage/SdCardController.h"
#include "../../services/storage/SecretStorage.h"
#include "../../media/MediaCache.h"
#include "AnimationStreamWorker.h"

// The image pipeline shared by the built-in Image Viewer and the Lua app
// host: source decode, aspect-fit and crop-zoom rendering, validated cache
// sidecars, and animation playback on the whole display. Its single owner
// drives it from the UI task and supplies the app bar through a painter;
// navigation and touch policy stay with that owner.
class ImageView {
public:
  using ChromePainter = void (*)(void *context);

  ImageView(DisplayController &display,
            SdCardController &sdCard,
            SecretStorage &secretStorage,
            MediaCache &mediaCache);

  // The painter draws the owner's app bar over the image whenever the view
  // repaints the top of the display while chrome is visible.
  void setChromePainter(ChromePainter painter, void *context);
  void open(const String &path, bool secretAccess, bool chromeVisible);
  void close();
  void tick(uint32_t nowMs);
  void setCropZoom(bool cropZoom);
  void setChromeVisible(bool visible);
  void redraw();

  bool ready() const { return _imageReady; }
  bool failed() const { return _failed; }
  const String &error() const { return _error; }
  const String &path() const { return _path; }
  String filename() const;
  uint32_t sourceWidth() const { return _sourceWidth; }
  uint32_t sourceHeight() const { return _sourceHeight; }
  const char *formatName() const;
  bool animated() const { return _animationSource; }
  bool cropZoom() const { return _cropZoom; }
  bool chromeVisible() const { return _viewMode == ViewMode::Informative; }
  bool zoomAvailable() const;

private:
  static const uint16_t MaxPaletteEntries = 256;
  static const uint16_t CacheWidth = 480;
  static const uint16_t CacheHeight = 320;
  static const uint16_t CacheHeaderSize = 32;
  static const uint32_t MaxPngScanlineBytes = 4096UL * 4UL;
  // Animations play in a centered photo viewport and are never upscaled.
  static const uint16_t AnimationMaxWidth = 320;
  static const uint16_t AnimationMaxHeight = 240;
  static const uint32_t CachePixelBytes = (uint32_t)CacheWidth * CacheHeight * 2UL;
  // A 16 KiB internal-RAM staging buffer amortizes FAT/SPI overhead while
  // leaving the shared touch bus responsive between transfers.
  static const uint16_t CacheChunkBytes = 16384;
  // Streamed frames normally go to the panel by DMA. When a clipped start row
  // is not 64-byte aligned, packed bands go to TFT_eSPI without staging.
  static const uint16_t StreamPresentBandRows = 48;
  // Fast opens show their first frame without flashing the loading panel.
  static const uint32_t StreamLoadingDelayMs = 150;
  static const uint32_t StreamLoadingRefreshMs = 50;
  static const uint32_t StreamStatsIntervalMs = 2000;

  enum class Compression : uint32_t {
    Rgb = 0,
    Rle8 = 1,
    Rle4 = 2,
    Bitfields = 3,
    AlphaBitfields = 6
  };

  enum class ImageFormat : uint8_t { Unknown, Bmp, Png, Jpeg, Gif, Webp };
  enum class ViewMode : uint8_t { Informative, FullScreen };

  struct PaletteEntry {
    uint8_t r;
    uint8_t g;
    uint8_t b;
  };

  DisplayController &_display;
  SdCardController &_sdCard;
  SecretStorage &_secretStorage;
  MediaCache &_mediaCache;
  StorageFile _file;
  StorageFile _decoderFile;
  MediaCache::Writer _cacheWriter;
  bool _secretAccess;
  uint8_t *_webpSource;
  uint32_t _webpSourceSize;
  uint8_t *_webpRgba;
  bool _animationSource;
  String _path;
  String _error;
  String _cachePath;
  String _cacheTempPath;
  String _zoomCachePath;
  String _zoomCacheTempPath;
  uint32_t _sourceFileSize;
  uint32_t _sourceWriteTime;
  uint32_t _cacheOffset;
  uint8_t _cacheChunk[CacheChunkBytes];
  ImageFormat _format;
  uint32_t _pixelOffset;
  uint32_t _sourceWidth;
  uint32_t _sourceHeight;
  uint32_t _rowStride;
  uint32_t _dibSize;
  uint16_t _bitsPerPixel;
  Compression _compression;
  bool _topDown;
  bool _hasAlpha;
  bool _rle;
  uint32_t _redMask;
  uint32_t _greenMask;
  uint32_t _blueMask;
  uint32_t _alphaMask;
  uint8_t _redShift;
  uint8_t _greenShift;
  uint8_t _blueShift;
  uint8_t _alphaShift;
  PaletteEntry _palette[MaxPaletteEntries];
  uint16_t _paletteCount;

  int16_t _left;
  int16_t _top;
  uint16_t _drawWidth;
  uint16_t _drawHeight;
  uint32_t _sourceCropX;
  uint32_t _sourceCropY;
  uint32_t _sourceCropWidth;
  uint32_t _sourceCropHeight;
  ViewMode _viewMode;
  bool _cropZoom;
  bool _imageReady;
  bool _writeCacheAfterRender;
  int32_t _nextDrawY;
  uint32_t _lastSourceY;
  uint16_t *_frameBuffer;
  uint32_t *_xCoordinates;
  uint8_t *_rawRow;
  uint8_t *_row0;
  uint8_t *_row1;
  bool _rendering;
  bool _cacheWriting;
  // Playback measurements logged to serial.
  uint32_t _streamPresentedFrames;
  uint32_t _streamStatsAtMs;
  uint32_t _openedAtMs;
  // A banded TFT_eSPI fallback presentation in progress.
  uint16_t _streamPresentY;
  bool _streamPresentActive;
  uint8_t _lastProgress;
  bool _failed = false;
  ChromePainter _chromePainter = nullptr;
  void *_chromeContext = nullptr;

  uint32_t _rleFileRow;
  uint32_t _rleX;
  uint32_t _rlePendingBlankRows;
  bool _rleDone;

  PNG _png;
  JPEGDEC _jpeg;
  // Animated GIF and WebP stream through this worker.
  AnimationStreamWorker _stream;
  uint16_t _streamSlot;
  const uint16_t *_streamPixels;
  uint32_t _streamLoadingAtMs;
  uint32_t _streamLoadingDrawnMs;
  // The displayed slot is being read by an LCD_CAM DMA transfer.
  bool _streamTransferActive;
  bool _streamDmaFallbackLogged;
  static ImageView *_decoderOwner;

  void reset();
  void draw();
  void drawChrome();
  void presentStaticFrame();
  StorageFile openSource(const char *path);
  bool isStaticImage() const;
  bool reloadStaticRender(bool cropZoom);
  void drawError();
  void drawLoading(uint8_t percent);
  void updateDecodeProgress(uint8_t percent);
  bool readHeader();
  bool readRasterHeader();
  bool prepareRender();
  bool decodeRasterImage();
  bool prepareCachePaths(FileFormats::Format format);
  bool loadCache(bool cropZoom);
  void prepareAnimationDrawRect();
  bool readGifStreamHeader();
  bool sniffAnimatedWebp();
  bool startAnimationStream(AnimationStreamWorker::Source source);
  bool tickAnimationStream(uint32_t nowMs);
  void stopAnimationStream();
  void beginStreamPresentation();
  void finishStreamTransfer();
  void tickStreamPresentation();
  void presentStreamFrameNow();
  void drawStreamLoading(uint32_t nowMs, const AnimationStreamWorker::Stats &stats);
  void clearStreamLoading();
  void logStreamStats(uint32_t nowMs, bool force = false);
  uint16_t streamFirstVisibleRow() const;
  bool isGifSource();
  bool isWebpSource();
  bool prepareWebpSource();
  bool inspectWebpSource();
  bool decodeWebpImage();
  void releaseWebpDecoder();
  void scaleWebpRgba(const uint8_t *rgba);
  bool cacheNeedsRotation() const;
  void startCacheWrite();
  void writeCacheChunk();
  void finishCacheWrite(bool success);
  bool writeCacheHeader();
  bool readCacheHeader(StorageFile &file, uint32_t &sourceWidth, uint32_t &sourceHeight) const;
  void writeLe16(uint8_t *buffer, uint16_t value) const;
  void writeLe32(uint8_t *buffer, uint32_t value) const;
  bool renderRows(uint8_t maxRows);
  bool readSourceRow(uint32_t sourceY, uint8_t *target);
  bool readUncompressedRow(uint32_t sourceY, uint8_t *target);
  bool readRleRow(uint8_t *target);
  bool readRleByte(uint8_t &value);
  void setPixel(uint8_t *row, uint32_t x, uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255);
  uint16_t blendPixel(const uint8_t *row0,
                      const uint8_t *row1,
                      uint32_t sourceX16,
                      uint32_t sourceY16) const;
  uint8_t extractChannel(uint32_t value, uint32_t mask, uint8_t shift) const;
  void releaseRenderScratch();
  void releaseBuffers();
  uint16_t readLe16(const uint8_t *buffer) const;
  uint32_t readLe32(const uint8_t *buffer) const;
  int32_t readLe32Signed(const uint8_t *buffer) const;

  static void *pngOpen(const char *name, int32_t *size);
  static void pngClose(void *handle);
  static int32_t pngRead(PNGFILE *file, uint8_t *buffer, int32_t length);
  static int32_t pngSeek(PNGFILE *file, int32_t position);
  static int pngDraw(PNGDRAW *draw);
  static void *jpegOpen(const char *name, int32_t *size);
  static void jpegClose(void *handle);
  static int32_t jpegRead(JPEGFILE *file, uint8_t *buffer, int32_t length);
  static int32_t jpegSeek(JPEGFILE *file, int32_t position);
  static int jpegDraw(JPEGDRAW *draw);
  void storeRasterPixel(uint32_t sourceX, uint32_t sourceY, uint16_t color);
};
