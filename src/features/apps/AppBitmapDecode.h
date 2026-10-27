#pragma once

#include <Arduino.h>

// Decodes an in-memory PNG or baseline JPEG into an RGB565 bitmap fitted
// inside a box, for SDK 6 apps. Runs on the app worker (`esp.bitmap.decode`)
// or the app's network task (`bitmap` responses); it never touches SD or the
// display. The returned pixels are in PSRAM; free them with heap_caps_free.
namespace AppBitmapDecode {
constexpr uint16_t MaxWidth = 320;
constexpr uint16_t MaxHeight = 432;
constexpr size_t MaxSourceBytes = 128 * 1024;

struct Options {
  uint16_t maxWidth = MaxWidth;
  uint16_t maxHeight = MaxHeight;
  uint16_t background = 0;  // RGB565 behind PNG alpha
};

bool supported(const uint8_t *data, size_t length);
// `maxBytes` caps the result's pixel bytes ("Bitmap memory full" above it).
uint16_t *decode(const uint8_t *data, size_t length, const Options &options, size_t maxBytes,
                 uint16_t &width, uint16_t &height, const char *&error);
}  // namespace AppBitmapDecode
