#pragma once

#include <Arduino.h>
#include <FS.h>

// JPEGDEC 1.8.x declares its filesystem overload with an unqualified File.
// ESP32's FS core keeps File in namespace fs, so provide the expected alias
// before including the decoder header.
using File = fs::File;

#ifndef PNG_MAX_BUFFERED_PIXELS
// PNGdec needs two uncompressed input rows. This supports common images up
// to 4096 pixels wide, including 32-bit RGBA PNG files.
#define PNG_MAX_BUFFERED_PIXELS ((4096 * 4 + 1) * 2)
#endif

#include <JPEGDEC.h>
#include <PNGdec.h>

#include "Screen.h"
#include "SdCardController.h"

class ImageViewerScreen : public Screen {
public:
  ImageViewerScreen(Navigator &navigator,
                    TouchController &touch,
                    DisplayController &display,
                    SdCardController &sdCard);

  void onEnter(const ScreenData &data) override;
  void onExit() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const uint16_t CloseTouchSize = 64;
  static const uint16_t MaxPaletteEntries = 256;
  static const uint16_t CacheWidth = 480;
  static const uint16_t CacheHeight = 320;
  static const uint16_t CacheHeaderSize = 24;
  static const uint32_t CachePixelBytes = (uint32_t)CacheWidth * CacheHeight * 2UL;
  static const uint16_t CacheChunkBytes = 4096;

  enum class Compression : uint32_t {
    Rgb = 0,
    Rle8 = 1,
    Rle4 = 2,
    Bitfields = 3,
    AlphaBitfields = 6
  };

  enum class ImageFormat : uint8_t { Unknown, Bmp, Png, Jpeg };

  struct PaletteEntry {
    uint8_t r;
    uint8_t g;
    uint8_t b;
  };

  SdCardController &_sdCard;
  fs::File _file;
  fs::File _decoderFile;
  fs::File _cacheFile;
  String _path;
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
  bool _lastTouchDown;
  uint8_t _lastProgress;

  uint32_t _rleFileRow;
  uint32_t _rleX;
  uint32_t _rlePendingBlankRows;
  bool _rleDone;

  PNG _png;
  JPEGDEC _jpeg;
  static ImageViewerScreen *_decoderOwner;

  void reset();
  void draw();
  void drawCloseIcon();
  void drawError();
  void drawLoading(uint8_t percent);
  void updateDecodeProgress(uint8_t percent);
  const char *formatName() const;
  bool readHeader();
  bool readRasterHeader();
  bool prepareRender();
  bool decodeRasterImage();
  String cachePathForSource() const;
  bool loadCache();
  bool cacheNeedsRotation() const;
  void startCacheWrite();
  void writeCacheChunk();
  void finishCacheWrite(bool success);
  bool writeCacheHeader(fs::File &file) const;
  bool readCacheHeader(fs::File &file) const;
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
  static int jpegDraw(JPEGDRAW *draw);
  void storeRasterPixel(uint32_t sourceX, uint32_t sourceY, uint16_t color);
};
