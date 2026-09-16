#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "../../services/assistant/AssistantState.h"

class AssistantAvatar {
public:
  void draw(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
            AssistantPhase phase, AssistantEmotion emotion,
            uint32_t nowMs, uint16_t backgroundColor) const;

private:
  static uint16_t phaseColor(AssistantPhase phase);
};

