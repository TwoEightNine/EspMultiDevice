#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

// Text handling used only by BrowserScreen. It deliberately keeps web-page
// text separate from the small ASCII-only fonts used by the rest of the UI.
namespace BrowserText {

enum class Size : uint8_t { Small, Normal, Large };

// Converts a textual HTTP response into well-formed UTF-8. `error` is set
// only when the page explicitly declares an unsupported charset.
bool normalizeToUtf8(const String &source, const String &contentType,
                     String &normalized, String &error);

// Starts and ends a single browser-only smooth Unicode font scope. Calls to
// draw() and width() also work outside a scope, but a scope avoids repeatedly
// loading glyph metrics while a page is laid out or painted.
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
