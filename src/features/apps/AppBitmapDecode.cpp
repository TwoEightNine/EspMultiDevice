#include "AppBitmapDecode.h"

#include <FS.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <new>

// JPEGDEC's filesystem overload refers to File without its fs namespace.
using File = fs::File;
#include <JPEGDEC.h>
#include <PNGdec.h>

#include "BitmapScale.h"

namespace {
// A JPEG decodes at a power-of-two reduction into this many pixels at most
// before the box filter brings it to the requested size.
constexpr uint32_t MaxIntermediatePixels = 640UL * 864UL;
// PNG rows are reduced as they arrive; only one source row is buffered.
constexpr uint16_t MaxPngWidth = 4096;

uint16_t *allocatePixels(size_t count) {
  return static_cast<uint16_t *>(
    heap_caps_calloc(count, sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

struct PngTarget {
  PNG *png;
  uint16_t *pixels;  // the result when no reduction is needed
  uint16_t *row;     // one source row when reducing
  BitmapScale::Reducer *reducer;
  uint16_t width;
  uint16_t height;
  uint32_t background;
};

int pngDraw(PNGDRAW *draw) {
  PngTarget *target = static_cast<PngTarget *>(draw->pUser);
  if (!target || draw->y < 0 || draw->y >= target->height || draw->iWidth != target->width) return 0;
  uint16_t *line = target->reducer ? target->row
                                   : target->pixels + static_cast<size_t>(draw->y) * target->width;
  target->png->getLineAsRGB565(draw, line, PNG_RGB565_LITTLE_ENDIAN, target->background);
  if (target->reducer) target->reducer->addRow(static_cast<uint16_t>(draw->y), line);
  return 1;
}

struct JpegTarget {
  uint16_t *pixels;
  uint16_t width;
  uint16_t height;
};

int jpegDraw(JPEGDRAW *draw) {
  JpegTarget *target = static_cast<JpegTarget *>(draw->pUser);
  if (!target) return 0;
  for (int row = 0; row < draw->iHeight; ++row) {
    const int y = draw->y + row;
    if (y < 0 || y >= target->height) continue;
    const int columns = std::min<int>(draw->iWidth, target->width - draw->x);
    if (columns <= 0) continue;
    memcpy(target->pixels + static_cast<size_t>(y) * target->width + draw->x,
           draw->pPixels + static_cast<size_t>(row) * draw->iWidth, columns * sizeof(uint16_t));
  }
  return 1;
}

uint16_t *decodePng(const uint8_t *data, size_t length, const AppBitmapDecode::Options &options,
                    size_t maxBytes, uint16_t &width, uint16_t &height, const char *&error) {
  void *storage = heap_caps_malloc(sizeof(PNG), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  PNG *decoder = storage ? new (storage) PNG() : nullptr;
  uint16_t *pixels = nullptr;
  uint16_t *row = nullptr;
  if (!decoder) {
    error = "App memory unavailable";
  } else if (decoder->openRAM(const_cast<uint8_t *>(data), static_cast<int>(length), pngDraw) != PNG_SUCCESS ||
             decoder->isInterlaced()) {
    error = "Unsupported image";
  } else {
    const int sourceWidth = decoder->getWidth();
    const int sourceHeight = decoder->getHeight();
    if (sourceWidth <= 0 || sourceHeight <= 0 || sourceWidth > MaxPngWidth || sourceHeight > 65535) {
      error = "Image too large";
    } else {
      BitmapScale::fit(static_cast<uint16_t>(sourceWidth), static_cast<uint16_t>(sourceHeight),
                       options.maxWidth, options.maxHeight, width, height);
      const bool reduce = width != sourceWidth || height != sourceHeight;
      BitmapScale::Reducer reducer;
      if (static_cast<size_t>(width) * height * 2 > maxBytes) error = "Bitmap memory full";
      else if (!(pixels = allocatePixels(static_cast<size_t>(width) * height)) ||
               (reduce && !(row = allocatePixels(static_cast<size_t>(sourceWidth)))) ||
               (reduce && !reducer.begin(static_cast<uint16_t>(sourceWidth),
                                         static_cast<uint16_t>(sourceHeight), pixels, width, height)))
        error = "App memory unavailable";
      else {
        // PNGdec blends alpha over a 0x00BBGGRR background.
        const uint32_t red = ((options.background >> 11) & 0x1f) * 255 / 31;
        const uint32_t green = ((options.background >> 5) & 0x3f) * 255 / 63;
        const uint32_t blue = (options.background & 0x1f) * 255 / 31;
        PngTarget target{decoder, pixels, row, reduce ? &reducer : nullptr,
                         static_cast<uint16_t>(sourceWidth), static_cast<uint16_t>(sourceHeight),
                         red | (green << 8) | (blue << 16)};
        if (decoder->decode(&target, PNG_CHECK_CRC) != PNG_SUCCESS) error = "Cannot decode image";
        else if (reduce) reducer.finish();
      }
    }
    decoder->close();
  }
  if (decoder) decoder->~PNG();
  heap_caps_free(storage);
  heap_caps_free(row);
  if (error) { heap_caps_free(pixels); return nullptr; }
  return pixels;
}

uint16_t *decodeJpeg(const uint8_t *data, size_t length, const AppBitmapDecode::Options &options,
                     size_t maxBytes, uint16_t &width, uint16_t &height, const char *&error) {
  void *storage = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  JPEGDEC *decoder = storage ? new (storage) JPEGDEC() : nullptr;
  uint16_t *pixels = nullptr;
  uint16_t *decoded = nullptr;
  if (!decoder) {
    error = "App memory unavailable";
  } else if (decoder->openRAM(const_cast<uint8_t *>(data), static_cast<int>(length), jpegDraw) != 1 ||
             decoder->getJPEGType() != JPEG_MODE_BASELINE) {
    error = "Unsupported image";
  } else {
    const int sourceWidth = decoder->getWidth();
    const int sourceHeight = decoder->getHeight();
    if (sourceWidth <= 0 || sourceHeight <= 0 || sourceWidth > 65535 || sourceHeight > 65535) {
      error = "Image too large";
    } else {
      BitmapScale::fit(static_cast<uint16_t>(sourceWidth), static_cast<uint16_t>(sourceHeight),
                       options.maxWidth, options.maxHeight, width, height);
      const uint8_t shift = BitmapScale::jpegShift(static_cast<uint16_t>(sourceWidth),
                                                   static_cast<uint16_t>(sourceHeight), width, height);
      const uint16_t decodedWidth = BitmapScale::reduced(static_cast<uint16_t>(sourceWidth), shift);
      const uint16_t decodedHeight = BitmapScale::reduced(static_cast<uint16_t>(sourceHeight), shift);
      const bool reduce = decodedWidth != width || decodedHeight != height;
      if (static_cast<size_t>(width) * height * 2 > maxBytes) error = "Bitmap memory full";
      else if (static_cast<uint32_t>(decodedWidth) * decodedHeight > MaxIntermediatePixels)
        error = "Image too large";
      else if (!(pixels = allocatePixels(static_cast<size_t>(width) * height)) ||
               (reduce && !(decoded = allocatePixels(static_cast<size_t>(decodedWidth) * decodedHeight))))
        error = "App memory unavailable";
      else {
        JpegTarget target{reduce ? decoded : pixels, decodedWidth, decodedHeight};
        decoder->setUserPointer(&target);
        decoder->setPixelType(RGB565_LITTLE_ENDIAN);
        const int scale = shift == 3 ? JPEG_SCALE_EIGHTH : shift == 2 ? JPEG_SCALE_QUARTER :
                          shift == 1 ? JPEG_SCALE_HALF : 0;
        if (decoder->decode(0, 0, scale) != 1) {
          error = "Cannot decode image";
        } else if (reduce) {
          BitmapScale::Reducer reducer;
          if (!reducer.begin(decodedWidth, decodedHeight, pixels, width, height)) {
            error = "App memory unavailable";
          } else {
            for (uint16_t y = 0; y < decodedHeight; ++y)
              reducer.addRow(y, decoded + static_cast<size_t>(y) * decodedWidth);
            reducer.finish();
          }
        }
      }
    }
    decoder->close();
  }
  if (decoder) decoder->~JPEGDEC();
  heap_caps_free(storage);
  heap_caps_free(decoded);
  if (error) { heap_caps_free(pixels); return nullptr; }
  return pixels;
}
}  // namespace

namespace AppBitmapDecode {
bool supported(const uint8_t *data, size_t length) {
  return (length >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) ||
         (length >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF);
}

uint16_t *decode(const uint8_t *data, size_t length, const Options &options, size_t maxBytes,
                 uint16_t &width, uint16_t &height, const char *&error) {
  width = height = 0;
  error = nullptr;
  if (!data || length > MaxSourceBytes) { error = "Image too large"; return nullptr; }
  if (!supported(data, length)) { error = "Unsupported image"; return nullptr; }
  Options bounded = options;
  bounded.maxWidth = std::min<uint16_t>(std::max<uint16_t>(bounded.maxWidth, 1), MaxWidth);
  bounded.maxHeight = std::min<uint16_t>(std::max<uint16_t>(bounded.maxHeight, 1), MaxHeight);
  const bool png = data[0] == 0x89;
  uint16_t *pixels = png ? decodePng(data, length, bounded, maxBytes, width, height, error)
                         : decodeJpeg(data, length, bounded, maxBytes, width, height, error);
  if (!pixels) width = height = 0;
  return pixels;
}
}  // namespace AppBitmapDecode
