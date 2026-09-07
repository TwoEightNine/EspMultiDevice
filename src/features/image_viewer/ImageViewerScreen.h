#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
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
#include "../../../third_party/AnimatedGIF/AnimatedGIF.h"
#include "../../../third_party/libwebp/src/webp/decode.h"
#include "../../../third_party/libwebp/src/webp/demux.h"

#include "../../core/Screen.h"
#include "../../services/storage/SdCardController.h"
#include "../../services/storage/SecretStorage.h"
#include "../../media/MediaCache.h"

class ImageViewerScreen : public Screen {
public:
  ImageViewerScreen(Navigator &navigator,
                    TouchController &touch,
                    DisplayController &display,
                    SdCardController &sdCard,
                    SecretStorage &secretStorage,
                    MediaCache &mediaCache);

  void onEnter(const ScreenData &data) override;
  void onExit() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const uint16_t MaxPaletteEntries = 256;
  static const uint16_t CacheWidth = 480;
  static const uint16_t CacheHeight = 320;
  static const uint16_t CacheHeaderSize = 24;
  static const uint16_t AnimationCacheHeaderSize = 40;
  static const uint32_t MaxPngScanlineBytes = 4096UL * 4UL;
  // Animated cache records use packed RGB444 pixels. They are expanded into
  // the RGB565 PSRAM ring by the worker without palette shimmer.
  static const uint32_t AnimationFrameHeaderSize = 4;
  static const uint16_t GifPhotoMaxWidth = 320;
  static const uint16_t GifPhotoMaxHeight = 240;
  static const uint32_t CachePixelBytes = (uint32_t)CacheWidth * CacheHeight * 2UL;
  // A 16 KiB internal-RAM staging buffer amortizes FAT/SPI overhead while
  // leaving the shared touch bus responsive between transfers.
  static const uint16_t CacheChunkBytes = 16384;
  static const uint32_t GifPsramReserveBytes = 512UL * 1024UL;
  static const uint8_t GifPreloadChunksPerTick = 2;
  static const uint8_t GifInitialPreloadFrames = 3;
  static const uint32_t GifMinimumDelayMs = 100;
  static const bool EnableBackgroundGifLoader = true;
  static const uint8_t GifPresentBandRows = 16;
  static const uint16_t GifLoaderReadChunkBytes = 2048;
  static const int16_t EdgeTapWidth = 48;
  static const int16_t CloseTapSize = 48;

  enum class Compression : uint32_t {
    Rgb = 0,
    Rle8 = 1,
    Rle4 = 2,
    Bitfields = 3,
    AlphaBitfields = 6
  };

  enum class ImageFormat : uint8_t { Unknown, Bmp, Png, Jpeg, Gif, Webp };

  struct PaletteEntry {
    uint8_t r;
    uint8_t g;
    uint8_t b;
  };

  enum class GifState : uint8_t { None, Building, Preloading, Playing };
  enum class GifSlotState : uint8_t { Free, Loading, Ready, Displaying };

  struct GifLoadRequest {
    uint32_t slot;
    uint32_t sequence;
  };

  struct GifLoadResult {
    uint32_t slot;
    uint32_t sequence;
    uint32_t delay;
    bool success;
  };

  SdCardController &_sdCard;
  SecretStorage &_secretStorage;
  MediaCache &_mediaCache;
  StorageFile _file;
  StorageFile _decoderFile;
  MediaCache::Writer _cacheWriter;
  StorageFile _gifCacheReadFile;
  bool _secretAccess;
  uint8_t *_webpSource;
  uint32_t _webpSourceSize;
  uint8_t *_webpRgba;
  WebPAnimDecoder *_webpAnimDecoder;
  WebPAnimInfo _webpAnimInfo;
  bool _webpAnimated;
  uint32_t _webpPreviousTimestamp;
  String _path;
  String _previousImagePath;
  String _nextImagePath;
  String _error;
  String _cachePath;
  String _cacheTempPath;
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
  int32_t _nextDrawY;
  uint32_t _lastSourceY;
  uint16_t *_frameBuffer;
  uint32_t *_xCoordinates;
  uint8_t *_rawRow;
  uint8_t *_row0;
  uint8_t *_row1;
  bool _rendering;
  bool _cacheWriting;
  GifState _gifState;
  bool _gifDecoderOpen;
  bool _gifBuildWritePending;
  bool _gifBuildLastFrame;
  bool _gifBuildFinalizing;
  uint32_t _gifBuildFrame;
  uint32_t _gifBuildWriteOffset;
  uint32_t _gifBuildDelay;
  uint32_t _gifFrameCount;
  uint32_t _gifPlaybackFrame;
  uint32_t _gifPlaybackDelay;
  uint32_t _gifNextDelay;
  uint32_t _gifCyclesCompleted;
  int32_t _gifLoopCount;
  uint16_t *_gifPlaybackA;
  uint16_t *_gifPlaybackB;
  uint32_t _gifBufferedFrame;
  uint32_t _gifBufferedOffset;
  bool _gifBufferedLoading;
  uint16_t *_gifResidentFrames;
  uint32_t *_gifResidentDelays;
  uint32_t *_gifResidentSequences;
  uint8_t *_gifResidentStates;
  uint32_t _gifResidentCapacity;
  uint32_t _gifPreloadTarget;
  uint32_t _gifPreloadSlot;
  uint32_t _gifPreloadSequence;
  uint32_t _gifPreloadOffset;
  uint32_t _gifNextLoadSequence;
  uint32_t _gifPlaybackSequence;
  bool _gifFullResident;
  bool _gifPreloadActive;
  QueueHandle_t _gifLoadRequests;
  QueueHandle_t _gifLoadResults;
  SemaphoreHandle_t _gifLoaderStopped;
  TaskHandle_t _gifLoaderTask;
  volatile bool _gifLoaderStop;
  bool _gifLoaderError;
  uint32_t _gifLoadedFrames;
  uint32_t _gifUnderruns;
  uint32_t _gifDroppedFrames;
  bool _gifWaitingForFrame;
  bool _gifActiveIsA;
  bool _gifNextLoaded;
  uint32_t _gifNextFrameAt;
  uint32_t _gifCanvasPixels;
  const uint16_t *_gifPresentFrame;
  uint16_t _gifPresentY;
  bool _gifPresentActive;
  uint16_t _gifPresentBuffer[GifPhotoMaxWidth * GifPresentBandRows];
  bool _lastTouchDown;
  uint8_t _lastProgress;

  uint32_t _rleFileRow;
  uint32_t _rleX;
  uint32_t _rlePendingBlankRows;
  bool _rleDone;

  PNG _png;
  JPEGDEC _jpeg;
  AnimatedGIF _gif;
  static ImageViewerScreen *_decoderOwner;

  void reset();
  void draw();
  void drawCloseIcon();
  void drawError();
  void drawLoading(uint8_t percent);
  void drawGifLoading(uint8_t percent);
  void drawGifPreloading(uint8_t percent);
  void updateDecodeProgress(uint8_t percent);
  const char *formatName() const;
  bool readHeader();
  bool readRasterHeader();
  bool prepareRender();
  bool decodeRasterImage();
  bool prepareCachePaths(FileFormats::Format format);
  bool loadCache();
  bool loadAnimationCache();
  bool readAnimationCacheHeader(StorageFile &file, uint32_t &frameCount, int32_t &loopCount,
                                uint32_t &sourceWidth, uint32_t &sourceHeight,
                                uint16_t &drawWidth, uint16_t &drawHeight) const;
  bool startGifBuild();
  bool buildGifFrame();
  bool finishGifBuild();
  bool startGifPlayback();
  bool tickGifPlayback(uint32_t nowMs);
  void beginGifTwoFrameLoad(uint32_t frameIndex);
  bool tickGifTwoFrameLoad();
  bool startGifPreload();
  bool tickGifPreload();
  bool beginGifResidentPlayback();
  bool startGifLoader();
  void stopGifLoader();
  bool enqueueGifLoad(uint32_t sequence);
  bool collectGifLoadResults();
  bool allGifPreloadSlotsReady() const;
  bool loadGifFrameOnWorker(StorageFile &cache, uint8_t *chunk,
                            const GifLoadRequest &request, GifLoadResult &result);
  void copyGifCacheChunk(uint16_t *destination, uint32_t pixelOffset,
                         const uint8_t *chunk, uint16_t amount);
  bool beginGifRingLoad(uint32_t sequence);
  bool tickGifRingLoad();
  bool startNextGifRingLoad();
  bool canPlayGifSequence(uint32_t sequence) const;
  uint32_t gifSourceFrameForSequence(uint32_t sequence) const;
  uint16_t *gifResidentFrame(uint32_t slot) const;
  uint32_t gifRgb444Bytes() const;
  uint32_t gifFrameRecordBytes() const;
  void prepareGifDrawRect();
  void presentGifFullFrame(const uint16_t *frame);
  void presentGifFrame(const uint16_t *frame, const uint16_t *previous, bool forceFull);
  bool tickGifPresentation();
  bool readGifCacheFrameChunk(uint32_t frameIndex, uint32_t pixelOffset,
                              uint16_t *destination, uint32_t &delay);
  bool readGifCacheFrame(uint32_t frameIndex, uint16_t *destination, uint32_t &delay);
  bool writeAnimationCacheHeader(uint32_t frameCount, int32_t loopCount);
  void releaseGifBuffers();
  void releaseGifDecoder();
  bool isGifSource();
  bool isWebpSource();
  bool prepareWebpSource();
  bool inspectWebpSource();
  bool decodeWebpImage();
  bool startWebpBuild();
  bool buildWebpFrame();
  bool finishWebpBuild();
  void releaseWebpDecoder();
  void scaleWebpRgba(const uint8_t *rgba);
  void scaleGifFrame();
  bool cacheNeedsRotation() const;
  void startCacheWrite();
  void writeCacheChunk();
  void finishCacheWrite(bool success);
  bool writeCacheHeader();
  bool readCacheHeader(StorageFile &file) const;
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
  static void *gifOpen(const char *name, int32_t *size);
  static void gifClose(void *handle);
  static int32_t gifRead(GIFFILE *file, uint8_t *buffer, int32_t length);
  static int32_t gifSeek(GIFFILE *file, int32_t position);
  static void gifDraw(GIFDRAW *draw);
  static void *gifAlloc(uint32_t size);
  static void gifFree(void *buffer);
  static void gifLoaderTask(void *context);
  void storeRasterPixel(uint32_t sourceX, uint32_t sourceY, uint16_t color);
};
