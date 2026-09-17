#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "../../services/assistant/AssistantState.h"

class AssistantAvatar {
public:
  enum class Asset : uint8_t { Waiting, Neutral, Sad, Happy, Speaking };

  static Asset assetFor(AssistantPhase phase, AssistantEmotion emotion);
  static const char *filename(Asset asset);

  static void drawUnderlay(TFT_eSPI &tft, int16_t x, int16_t y,
                           const uint16_t *pixels, const uint8_t *alpha);
  void draw(TFT_eSPI &tft, int16_t x, int16_t y,
            const uint16_t *pixels, uint16_t backgroundColor,
            const uint8_t *alpha = nullptr,
            const uint16_t *underlayPixels = nullptr) const;

private:
  static constexpr uint16_t Width = 320;
  static constexpr uint16_t Height = 240;
};
