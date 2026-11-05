#include "AppBitmapDecode.h"

#include <algorithm>

#include "../../media/BitmapDecode.h"

namespace AppBitmapDecode {
bool supported(const uint8_t *data, size_t length) {
  return BitmapDecode::supported(data, length);
}

uint16_t *decode(const uint8_t *data, size_t length, const Options &options, size_t maxBytes,
                 uint16_t &width, uint16_t &height, const char *&error) {
  BitmapDecode::Options bounded;
  bounded.maxWidth = std::min<uint16_t>(std::max<uint16_t>(options.maxWidth, 1), MaxWidth);
  bounded.maxHeight = std::min<uint16_t>(std::max<uint16_t>(options.maxHeight, 1), MaxHeight);
  bounded.background = options.background;
  bounded.maxSourceBytes = MaxSourceBytes;
  bounded.maxPixelBytes = maxBytes;
  uint16_t *pixels = BitmapDecode::decode(data, length, bounded, width, height, error);
  // Apps document this wording for an allocation failure.
  if (error == BitmapDecode::OutOfMemory) error = "App memory unavailable";
  return pixels;
}
}  // namespace AppBitmapDecode
