#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

namespace UiKit {

constexpr int16_t AppBarHeight = 48;
constexpr int16_t NavigationIconSize = 40;
constexpr int16_t ContentPadding = 16;
constexpr int16_t ListCellHeight = 40;
constexpr int16_t ListIconSize = 32;
constexpr uint16_t AppBarColor = 0x1082;  // #111111 in RGB565.
constexpr uint16_t DividerColor = TFT_DARKGREY;
constexpr uint8_t TitleFont = 4;
constexpr uint8_t ListFont = 2;

enum class NavigationIcon : uint8_t {
  Back,
  Close
};

bool navigationIconContains(int16_t x, int16_t y);

String ellipsize(TFT_eSPI &tft, const String &text, int16_t maxWidth, uint8_t font);

void drawTopAppBar(TFT_eSPI &tft,
                   int16_t width,
                   const String &title,
                   NavigationIcon icon = NavigationIcon::Back);

void drawScaledXBitmap(TFT_eSPI &tft,
                       int16_t x,
                       int16_t y,
                       const uint8_t *bitmap,
                       uint8_t bitmapWidth,
                       uint8_t bitmapHeight,
                       uint16_t color,
                       uint8_t scale = 2);

void drawListCell(TFT_eSPI &tft,
                  int16_t width,
                  int16_t y,
                  const String &label,
                  const uint8_t *icon = nullptr,
                  uint16_t iconColor = TFT_WHITE,
                  const String &trailingText = String());

}  // namespace UiKit
