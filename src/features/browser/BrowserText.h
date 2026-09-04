#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "../../ui/UiText.h"

// Browser-specific text decoding plus compatibility wrappers around the shared
// UI renderer used for document text.
namespace BrowserText {

using Size = UiText::Size;

// Converts a textual HTTP response into well-formed UTF-8. `error` is set
// only when the page explicitly declares an unsupported charset.
bool normalizeToUtf8(const String &source, const String &contentType,
                     String &normalized, String &error);

// Starts and ends a shared smooth Unicode font scope. Calls to draw() and
// width() also work outside a scope, but a scope avoids repeated font loads.
void begin(TFT_eSPI &tft, Size size = Size::Normal);
void end(TFT_eSPI &tft);

int16_t lineHeight(Size size);
// Heap-free layout measurement. Unlike width(), this never loads a TFT font.
int16_t measure(const String &text, Size size = Size::Normal);
int16_t width(TFT_eSPI &tft, const String &text, Size size = Size::Normal);
void draw(TFT_eSPI &tft, const String &text, int16_t x, int16_t y,
          uint16_t color, uint16_t background, Size size = Size::Normal);
String ellipsize(TFT_eSPI &tft, const String &text, int16_t maxWidth,
                 Size size = Size::Normal);

}  // namespace BrowserText
