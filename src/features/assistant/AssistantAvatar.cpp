#include "AssistantAvatar.h"

#include <math.h>

uint16_t AssistantAvatar::phaseColor(AssistantPhase phase) {
  if (phase == AssistantPhase::Error) return 0xf986;
  if (phase == AssistantPhase::NeedsActivation) return 0xfd20;
  if (phase == AssistantPhase::Listening) return TFT_CYAN;
  if (phase == AssistantPhase::Thinking) return 0x9d7f;
  if (phase == AssistantPhase::Speaking) return 0x67ec;
  return 0x6dbf;
}

void AssistantAvatar::draw(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                           AssistantPhase phase, AssistantEmotion emotion,
                           uint32_t nowMs, uint16_t backgroundColor) const {
  const int16_t left = centerX - 92;
  const int16_t top = centerY - 62;
  tft.fillRect(left, top, 184, 128, backgroundColor);
  const uint16_t color = phaseColor(phase);
  const uint8_t beat = static_cast<uint8_t>((nowMs / 100U) % 10U);
  const int16_t pulse = phase == AssistantPhase::Listening
      ? static_cast<int16_t>(beat <= 5 ? beat : 10 - beat) : 0;
  tft.drawSmoothCircle(centerX, centerY, 49 + pulse, color, backgroundColor);
  tft.fillSmoothCircle(centerX, centerY, 43, 0x18e3, backgroundColor);

  const int16_t eyeY = centerY - 10;
  const int16_t eyeOffset = 16;
  if (emotion == AssistantEmotion::Happy) {
    tft.drawWideLine(centerX - eyeOffset - 5, eyeY + 2,
                     centerX - eyeOffset, eyeY - 2, 3, color, 0x18e3);
    tft.drawWideLine(centerX - eyeOffset, eyeY - 2,
                     centerX - eyeOffset + 5, eyeY + 2, 3, color, 0x18e3);
    tft.drawWideLine(centerX + eyeOffset - 5, eyeY + 2,
                     centerX + eyeOffset, eyeY - 2, 3, color, 0x18e3);
    tft.drawWideLine(centerX + eyeOffset, eyeY - 2,
                     centerX + eyeOffset + 5, eyeY + 2, 3, color, 0x18e3);
  } else {
    tft.fillSmoothCircle(centerX - eyeOffset, eyeY, 4, color, 0x18e3);
    tft.fillSmoothCircle(centerX + eyeOffset, eyeY, 4, color, 0x18e3);
  }

  int16_t mouthHalf = 11;
  int16_t mouthY = centerY + 17;
  if (phase == AssistantPhase::Speaking) {
    mouthHalf = 7 + static_cast<int16_t>((nowMs / 80U) % 7U);
    const int16_t height = 4 + static_cast<int16_t>((nowMs / 100U) % 5U);
    tft.fillSmoothRoundRect(centerX - mouthHalf, mouthY - height / 2,
                            mouthHalf * 2, height, 2, color, 0x18e3);
    const int16_t wave = 55 + static_cast<int16_t>((nowMs / 120U) % 9U);
    tft.drawSmoothCircle(centerX, centerY, wave, color, backgroundColor);
  } else if (phase == AssistantPhase::Error ||
             emotion == AssistantEmotion::Sad) {
    tft.drawWideLine(centerX - mouthHalf, mouthY + 3, centerX, mouthY - 2,
                     3, color, 0x18e3);
    tft.drawWideLine(centerX, mouthY - 2, centerX + mouthHalf, mouthY + 3,
                     3, color, 0x18e3);
  } else {
    tft.drawWideLine(centerX - mouthHalf, mouthY, centerX + mouthHalf, mouthY,
                     3, color, 0x18e3);
  }

  if (phase == AssistantPhase::Thinking) {
    for (uint8_t index = 0; index < 3; ++index) {
      const float angle = (static_cast<float>(nowMs % 1800U) / 1800.0f) *
                          6.2831853f + index * 2.0943951f;
      const int16_t x = centerX + static_cast<int16_t>(cosf(angle) * 59.0f);
      const int16_t y = centerY + static_cast<int16_t>(sinf(angle) * 59.0f);
      tft.fillSmoothCircle(x, y, 3, color, backgroundColor);
    }
  }
}

