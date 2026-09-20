#include "AssistantAvatar.h"

AssistantAvatar::Asset AssistantAvatar::assetFor(AssistantPhase phase,
                                                 AssistantEmotion emotion) {
  if (phase == AssistantPhase::Error ||
      phase == AssistantPhase::NeedsWiFi ||
      phase == AssistantPhase::Bootstrapping ||
      phase == AssistantPhase::NeedsActivation ||
      phase == AssistantPhase::Connecting ||
      phase == AssistantPhase::Thinking) {
    return Asset::Waiting;
  }
  if (phase == AssistantPhase::Speaking) return Asset::Speaking;
  if (emotion == AssistantEmotion::Happy) return Asset::Happy;
  if (emotion == AssistantEmotion::Sad) return Asset::Sad;
  return Asset::Neutral;
}

const char *AssistantAvatar::filename(Asset asset) {
  switch (asset) {
    case Asset::Waiting: return "w.png";
    case Asset::Sad: return "s.png";
    case Asset::Happy: return "h.png";
    case Asset::Speaking: return "t.png";
    case Asset::Neutral:
    default: return "n.png";
  }
}

void AssistantAvatar::clear(TFT_eSPI &tft, int16_t x, int16_t y,
                            uint16_t backgroundColor) {
  tft.fillRect(x, y, Width, Height, backgroundColor);
}

void AssistantAvatar::drawUnderlay(TFT_eSPI &tft, int16_t x, int16_t y,
                                   const uint16_t *pixels,
                                   const uint8_t *alpha) {
  if (!pixels || !alpha) return;
  uint16_t line[Width];
  for (int16_t row = 0; row < Height; ++row) {
    const size_t offset = static_cast<size_t>(row) * Width;
    int16_t runStart = -1;
    for (int16_t column = 0; column <= Width; ++column) {
      const bool opaque = column < Width && alpha[offset + column] != 0;
      if (opaque && runStart < 0) {
        runStart = column;
      } else if (!opaque && runStart >= 0) {
        const int16_t runLength = column - runStart;
        for (int16_t index = 0; index < runLength; ++index) {
          const size_t source = offset + runStart + index;
          line[index] = pixels[source];
        }
        tft.pushImage(x + runStart, y + row, runLength, 1, line);
        runStart = -1;
      }
    }
  }
}

void AssistantAvatar::draw(TFT_eSPI &tft, int16_t x, int16_t y,
                           const uint16_t *pixels,
                           uint16_t backgroundColor,
                           const uint8_t *alpha,
                           const uint16_t *underlayPixels) const {
  if (!pixels) return;
  if (!alpha) {
    (void)backgroundColor;
    tft.pushImage(x, y, Width, Height, pixels);
    return;
  }

  uint16_t line[Width];
  for (int16_t row = 0; row < Height; ++row) {
    const size_t offset = static_cast<size_t>(row) * Width;
    int16_t runStart = -1;
    for (int16_t column = 0; column <= Width; ++column) {
      const bool opaque = column < Width && alpha[offset + column] != 0;
      if (opaque && runStart < 0) {
        runStart = column;
      } else if (!opaque && runStart >= 0) {
        const int16_t runLength = column - runStart;
        for (int16_t index = 0; index < runLength; ++index) {
          const size_t source = offset + runStart + index;
          const uint8_t pixelAlpha = alpha[source];
          const uint16_t background = underlayPixels ? underlayPixels[source] : backgroundColor;
          if (pixelAlpha == 255) {
            line[index] = pixels[source];
          } else {
            const uint16_t inverse = 255 - pixelAlpha;
            const uint16_t red = static_cast<uint16_t>(
                ((pixels[source] >> 11) * pixelAlpha +
                 (background >> 11) * inverse + 127) / 255);
            const uint16_t green = static_cast<uint16_t>(
                ((((pixels[source] >> 5) & 0x3f) * pixelAlpha) +
                 (((background >> 5) & 0x3f) * inverse) + 127) / 255);
            const uint16_t blue = static_cast<uint16_t>(
                (((pixels[source] & 0x1f) * pixelAlpha) +
                 ((background & 0x1f) * inverse) + 127) / 255);
            line[index] = static_cast<uint16_t>((red << 11) | (green << 5) | blue);
          }
        }
        tft.pushImage(x + runStart, y + row, runLength, 1, line);
        runStart = -1;
      }
    }
  }
}
