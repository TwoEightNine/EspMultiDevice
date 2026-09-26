#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "../../services/assistant/AssistantState.h"

class AssistantAvatar {
public:
  enum class Asset : uint8_t { Waiting, Neutral, Sad, Happy, Speaking };

  static Asset assetFor(AssistantPhase phase, AssistantEmotion emotion);
  static const char *filename(Asset asset);

  // Owns clearing the complete native avatar frame before a new asset set is
  // available. Text chrome must not paint this region.
  static void clear(TFT_eSPI &tft, int16_t x, int16_t y,
                    uint16_t backgroundColor);
  void draw(TFT_eSPI &tft, int16_t x, int16_t y,
            const uint16_t *pixels, uint16_t backgroundColor,
            const uint8_t *alpha = nullptr,
            const uint16_t *backgroundPixels = nullptr) const;

private:
  static constexpr uint16_t Width = 320;
  static constexpr uint16_t Height = 240;
};
