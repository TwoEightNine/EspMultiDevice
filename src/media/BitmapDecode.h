#pragma once

#include <Arduino.h>

// Decodes an in-memory PNG or baseline JPEG into an RGB565 bitmap fitted
// inside a box, with the aspect ratio kept and never scaled up. Shared by SDK 6
// apps (through AppBitmapDecode) and the Audio Player's cover worker. It never
// touches SD or the display, so it can run on any task. The returned pixels
// are in PSRAM; free them with heap_caps_free.
//
// A JPEG decodes at the smallest JPEGDEC reduction that still covers the box,
// at most 640x864 pixels, and BitmapScale's box filter brings it to size. A
// PNG (non-interlaced, at most 4,096 px wide) is reduced row by row, with alpha
// blended over `background`.
namespace BitmapDecode {
// The error for a failed allocation; callers may map it to their own wording.
extern const char *const OutOfMemory;

struct Options {
  uint16_t maxWidth = 1;
  uint16_t maxHeight = 1;
  uint16_t background = 0;    // RGB565 behind PNG alpha
  size_t maxSourceBytes = 0;  // "Image too large" above it
  size_t maxPixelBytes = 0;   // "Bitmap memory full" above it
};

// True for the PNG and JPEG signatures, as FileFormats classifies them.
bool supported(const uint8_t *data, size_t length);
uint16_t *decode(const uint8_t *data, size_t length, const Options &options,
                 uint16_t &width, uint16_t &height, const char *&error);
}  // namespace BitmapDecode
