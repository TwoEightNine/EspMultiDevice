#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// Shared smooth FreeSans renderer for all application UI text.  The font
// faces include Latin and Cyrillic glyphs at the three visual hierarchy sizes.
namespace UiText {

enum class Size : uint8_t {
  Compact,
  Content,
  Title,
  // Compatibility aliases for Browser document style parsing.
  Small = Compact,
  Normal = Content,
  Large = Title
};

void begin(TFT_eSPI &tft, Size size = Size::Content);
void end(TFT_eSPI &tft);

int16_t lineHeight(Size size);
// Heap-free measurement; does not activate a TFT font.
int16_t measure(const String &text, Size size = Size::Content);
int16_t width(TFT_eSPI &tft, const String &text, Size size = Size::Content);
String ellipsize(const String &text, int16_t maxWidth, Size size = Size::Content);
// Replaces unsupported glyphs and maps common typographic punctuation to the
// nearest supported form without modifying the underlying application text.
String normalizeForFont(const String &text, Size size = Size::Content);

// Assistant captions use the normal UI face unless they contain Han text, in
// which case they use the embedded 18 px Simplified-Chinese face. These helpers
// keep wrapping and drawing on the same face.
String normalizeAssistantCaption(const String &text);
int16_t measureAssistantCaption(const String &text);
String ellipsizeAssistantCaption(const String &text, int16_t maxWidth);

// Draws with the requested alignment and restores TFT_eSPI's normal text
// state after a temporary font scope has finished.
void draw(TFT_eSPI &tft,
          const String &text,
          int16_t x,
          int16_t y,
          uint16_t color,
          uint16_t background,
          Size size = Size::Content,
          uint8_t datum = TL_DATUM);

void drawAssistantCaption(TFT_eSPI &tft,
                          const String &text,
                          int16_t x,
                          int16_t y,
                          uint16_t color,
                          uint16_t background,
                          uint8_t datum = TL_DATUM);

}  // namespace UiText
