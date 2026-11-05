#pragma once

#include <stddef.h>
#include <stdint.h>

// Arduino-free sizing and downscaling for BitmapDecode (SDK 6 app bitmaps
// and Audio Player covers). Pixels are native
// RGB565 values. tests/bitmap_scale covers the contract on the host.
namespace BitmapScale {

// The largest size that fits inside `maxWidth`×`maxHeight` with the source
// aspect ratio kept, never larger than the source and never below 1×1.
void fit(uint16_t width, uint16_t height, uint16_t maxWidth, uint16_t maxHeight,
         uint16_t &outWidth, uint16_t &outHeight);

// The JPEG decode reduction as a shift (0 = full size … 3 = 1/8): the largest
// one whose output, rounded up, still covers `targetWidth`×`targetHeight`.
uint8_t jpegShift(uint16_t width, uint16_t height, uint16_t targetWidth, uint16_t targetHeight);

// Size of a dimension after a JPEG decode reduction, rounded up.
inline uint16_t reduced(uint16_t value, uint8_t shift) {
  return static_cast<uint16_t>((static_cast<uint32_t>(value) + (1u << shift) - 1) >> shift);
}

// Box-filter reduction fed one source row at a time, in order. Each source
// pixel is averaged into exactly one target pixel, so a target dimension
// must not exceed the source one.
class Reducer {
public:
  Reducer() = default;
  ~Reducer();
  Reducer(const Reducer &) = delete;
  Reducer &operator=(const Reducer &) = delete;

  bool begin(uint16_t sourceWidth, uint16_t sourceHeight, uint16_t *target,
             uint16_t targetWidth, uint16_t targetHeight);
  // Rows outside 0…sourceHeight-1 or out of order are ignored.
  void addRow(uint16_t y, const uint16_t *row);
  // Writes the last pending target row.
  void finish();

private:
  uint32_t *_sums = nullptr;  // r, g, b, count per target column
  uint16_t *_target = nullptr;
  uint16_t _sourceWidth = 0;
  uint16_t _sourceHeight = 0;
  uint16_t _targetWidth = 0;
  uint16_t _targetHeight = 0;
  int32_t _row = -1;      // target row being accumulated
  int32_t _lastY = -1;

  void flush();
};

}  // namespace BitmapScale
