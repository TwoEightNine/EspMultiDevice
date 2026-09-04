#include "UiKit.h"

namespace UiKit {
namespace {
constexpr int16_t NavigationIconLeft = AppBarIconPadding;
constexpr int16_t NavigationIconTop = AppBarIconPadding;
constexpr int16_t TitleX = NavigationIconLeft + NavigationIconSize + AppBarIconPadding;
// Font 4's visible glyphs sit slightly high within its 26 px cell.
constexpr int16_t TitleCenterY = AppBarHeight / 2 + 1;
constexpr int16_t ListIconX = 8;
constexpr int16_t ListIconY = (ListCellHeight - ListIconSize) / 2;
constexpr int16_t ListTextWithIconX = ListIconX + ListIconSize + ContentPadding;

}  // namespace

bool navigationIconContains(int16_t x, int16_t y) {
  return x >= 0 && x < AppBarHeight && y >= 0 && y < AppBarHeight;
}

String ellipsize(const String &text, int16_t maxWidth, UiText::Size size) {
  return UiText::ellipsize(text, maxWidth, size);
}

void drawTopAppBar(TFT_eSPI &tft,
                   int16_t width,
                   const String &title,
                   NavigationIcon icon) {
  tft.fillRect(0, 0, width, AppBarHeight, AppBarColor);
  const int16_t iconCenterX = NavigationIconLeft + NavigationIconSize / 2;
  const int16_t iconCenterY = NavigationIconTop + NavigationIconSize / 2;
  if (icon == NavigationIcon::Back) {
    drawBackIcon(tft, iconCenterX, iconCenterY, TFT_WHITE, AppBarColor);
  } else {
    drawCloseIcon(tft, iconCenterX, iconCenterY, TFT_WHITE, AppBarColor);
  }

  const int16_t titleWidth = width - TitleX - ContentPadding;
  if (titleWidth > 0 && title.length() > 0) {
    UiText::draw(tft, UiKit::ellipsize(title, titleWidth, UiText::Size::Title),
                 TitleX, TitleCenterY, TFT_WHITE, AppBarColor,
                 UiText::Size::Title, ML_DATUM);
  }
}

void drawCloseIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                   uint16_t color, uint16_t backgroundColor) {
  constexpr float strokeWidth = 3.0f;
  constexpr float strokeRadius = strokeWidth / 2.0f;
  constexpr float halfSize = NavigationIconSize / 2.0f;
  const float first = halfSize - strokeRadius;
  tft.drawWideLine(centerX - first, centerY - first,
                   centerX + first, centerY + first,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX + first, centerY - first,
                   centerX - first, centerY + first,
                   strokeWidth, color, backgroundColor);
}

void drawBackIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor) {
  constexpr float strokeWidth = 3.0f;
  constexpr float wing = 8.0f;
  tft.drawWideLine(centerX + 10, centerY, centerX - 10, centerY,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX - 10, centerY, centerX, centerY - wing,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX - 10, centerY, centerX, centerY + wing,
                   strokeWidth, color, backgroundColor);
}

void drawPageArrow(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                   bool vertical, bool forward, uint16_t color,
                   uint16_t backgroundColor) {
  constexpr float strokeWidth = 3.0f;
  constexpr float tipOffset = 9.0f;
  constexpr float tailOffset = 10.0f;
  constexpr float wing = 8.0f;
  if (vertical) {
    const float tipY = centerY + (forward ? tipOffset : -tipOffset);
    const float tailY = centerY - (forward ? tailOffset : -tailOffset);
    const float wingY = centerY + (forward ? 1.0f : -1.0f);
    tft.drawWideLine(centerX, tailY, centerX, tipY, strokeWidth, color, backgroundColor);
    tft.drawWideLine(centerX, tipY, centerX - wing, wingY, strokeWidth, color, backgroundColor);
    tft.drawWideLine(centerX, tipY, centerX + wing, wingY, strokeWidth, color, backgroundColor);
    return;
  }

  const float tipX = centerX + (forward ? tipOffset : -tipOffset);
  const float tailX = centerX - (forward ? tailOffset : -tailOffset);
  const float wingX = centerX + (forward ? 1.0f : -1.0f);
  tft.drawWideLine(tailX, centerY, tipX, centerY, strokeWidth, color, backgroundColor);
  tft.drawWideLine(tipX, centerY, wingX, centerY - wing, strokeWidth, color, backgroundColor);
  tft.drawWideLine(tipX, centerY, wingX, centerY + wing, strokeWidth, color, backgroundColor);
}

void drawPlusIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor) {
  constexpr float strokeWidth = 3.0f;
  constexpr float halfLength = 9.0f;
  tft.drawWideLine(centerX - halfLength, centerY, centerX + halfLength, centerY,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX, centerY - halfLength, centerX, centerY + halfLength,
                   strokeWidth, color, backgroundColor);
}

void drawHomeIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor) {
  constexpr float strokeWidth = 3.0f;
  const int16_t left = centerX - 10;
  const int16_t right = centerX + 10;
  const int16_t roof = centerY - 10;
  const int16_t wallTop = centerY - 1;
  const int16_t base = centerY + 10;
  tft.drawWideLine(left, wallTop, centerX, roof, strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX, roof, right, wallTop, strokeWidth, color, backgroundColor);
  tft.drawWideLine(left, wallTop, left, base, strokeWidth, color, backgroundColor);
  tft.drawWideLine(right, wallTop, right, base, strokeWidth, color, backgroundColor);
  tft.drawWideLine(left, base, right, base, strokeWidth, color, backgroundColor);
  tft.fillSmoothRoundRect(centerX - 3, centerY + 2, 6, 8, 1, color, backgroundColor);
}

void drawRecentIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                    uint16_t color, uint16_t backgroundColor) {
  tft.drawSmoothCircle(centerX, centerY, 9, color, backgroundColor);
  tft.drawWideLine(centerX, centerY, centerX, centerY - 6,
                   2.5f, color, backgroundColor);
  tft.drawWideLine(centerX, centerY, centerX + 5, centerY + 3,
                   2.5f, color, backgroundColor);
}

void drawFavoriteIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                      uint16_t color, uint16_t backgroundColor) {
  constexpr int16_t points[][2] = {{0, -10}, {3, -3}, {10, -3}, {5, 2}, {7, 9},
                                    {0, 5}, {-7, 9}, {-5, 2}, {-10, -3}, {-3, -3}};
  constexpr float strokeWidth = 2.5f;
  for (uint8_t i = 0; i < 10; i++) {
    const uint8_t next = (i + 1) % 10;
    tft.drawWideLine(centerX + points[i][0], centerY + points[i][1],
                     centerX + points[next][0], centerY + points[next][1],
                     strokeWidth, color, backgroundColor);
  }
}

void drawMoreIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor) {
  tft.fillSmoothCircle(centerX, centerY - 7, 2, color, backgroundColor);
  tft.fillSmoothCircle(centerX, centerY, 2, color, backgroundColor);
  tft.fillSmoothCircle(centerX, centerY + 7, 2, color, backgroundColor);
}

void drawListCloseIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                       uint16_t color, uint16_t backgroundColor) {
  constexpr float strokeWidth = 2.0f;
  constexpr float strokeRadius = strokeWidth / 2.0f;
  constexpr float halfSize = ListActionIconSize / 2.0f;
  const float extent = halfSize - strokeRadius;
  tft.drawWideLine(centerX - extent, centerY - extent,
                   centerX + extent, centerY + extent,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX + extent, centerY - extent,
                   centerX - extent, centerY + extent,
                   strokeWidth, color, backgroundColor);
}

void drawListMoreIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                      uint16_t color, uint16_t backgroundColor) {
  constexpr int16_t dotRadius = 2;
  constexpr int16_t dotOffset = ListActionIconSize / 2 - dotRadius;
  tft.fillSmoothCircle(centerX, centerY - dotOffset, dotRadius, color, backgroundColor);
  tft.fillSmoothCircle(centerX, centerY, dotRadius, color, backgroundColor);
  tft.fillSmoothCircle(centerX, centerY + dotOffset, dotRadius, color, backgroundColor);
}

void drawKeyboardBackspaceIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                               uint16_t color, uint16_t backgroundColor) {
  constexpr float strokeWidth = 2.0f;
  constexpr float extent = KeyboardIconSize / 2.0f - strokeWidth / 2.0f;
  tft.drawWideLine(centerX - extent, centerY, centerX + extent, centerY,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX - extent, centerY, centerX - 3, centerY - 4,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX - extent, centerY, centerX - 3, centerY + 4,
                   strokeWidth, color, backgroundColor);
}

void drawKeyboardEnterIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                           uint16_t color, uint16_t backgroundColor) {
  constexpr float strokeWidth = 2.0f;
  constexpr float extent = KeyboardIconSize / 2.0f - strokeWidth / 2.0f;
  tft.drawWideLine(centerX + 4, centerY - extent, centerX + 4, centerY + 3,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX + 4, centerY + 3, centerX - extent, centerY + 3,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX - extent, centerY + 3, centerX - 3, centerY - 1,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX - extent, centerY + 3, centerX - 3, centerY + extent,
                   strokeWidth, color, backgroundColor);
}

void drawKeyboardShiftIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                           bool capsLock, uint16_t color, uint16_t backgroundColor) {
  constexpr float strokeWidth = 2.0f;
  constexpr float extent = KeyboardIconSize / 2.0f - strokeWidth / 2.0f;
  tft.drawWideLine(centerX, centerY + extent, centerX, centerY - 3,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX, centerY - extent, centerX - 6, centerY - 1,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX, centerY - extent, centerX + 6, centerY - 1,
                   strokeWidth, color, backgroundColor);
  if (capsLock) {
    tft.fillSmoothCircle(centerX, centerY + 5, 1, color, backgroundColor);
  }
}

void drawScaledXBitmap(TFT_eSPI &tft,
                       int16_t x,
                       int16_t y,
                       const uint8_t *bitmap,
                       uint8_t bitmapWidth,
                       uint8_t bitmapHeight,
                       uint16_t color,
                       uint8_t scale) {
  if (!bitmap || scale == 0) return;
  const uint8_t rowBytes = (bitmapWidth + 7) / 8;
  for (uint8_t row = 0; row < bitmapHeight; row++) {
    for (uint8_t column = 0; column < bitmapWidth; column++) {
      uint8_t value = pgm_read_byte(bitmap + row * rowBytes + column / 8);
      if ((value & (1 << (column & 7))) == 0) continue;
      tft.fillRect(x + column * scale, y + row * scale, scale, scale, color);
    }
  }
}

void drawListCell(TFT_eSPI &tft,
                  int16_t width,
                  int16_t y,
                  const String &label,
                  const uint8_t *icon,
                  uint16_t iconColor,
                  const String &trailingText) {
  tft.fillRect(0, y, width, ListCellHeight, TFT_BLACK);
  const int16_t textX = icon ? ListTextWithIconX : ContentPadding;
  if (icon) {
    drawScaledXBitmap(tft, ListIconX, y + ListIconY, icon, 16, 16, iconColor);
  }

  int16_t trailingWidth = 0;
  if (trailingText.length() > 0) {
    trailingWidth = UiText::measure(trailingText, UiText::Size::Compact);
    UiText::draw(tft, trailingText, width - ContentPadding, y + ListCellHeight / 2,
                 TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact, MR_DATUM);
  }

  const int16_t labelEnd = width - ContentPadding -
                           (trailingWidth > 0 ? trailingWidth + 8 : 0);
  UiText::draw(tft, UiKit::ellipsize(label, labelEnd - textX, UiText::Size::Content),
               textX, y + ListCellHeight / 2, TFT_WHITE, TFT_BLACK,
               UiText::Size::Content, ML_DATUM);
  tft.drawFastHLine(textX, y + ListCellHeight - 1,
                    width - ContentPadding - textX, DividerColor);
}

void drawProgressBar(TFT_eSPI &tft,
                     int16_t x,
                     int16_t y,
                     int16_t width,
                     int16_t height,
                     uint8_t value,
                     bool indeterminate,
                     uint16_t fillColor,
                     uint16_t frameColor,
                     uint16_t backgroundColor,
                     bool drawFrame,
                     uint32_t indeterminateElapsedMs) {
  if (width < 5 || height < 5) return;
  const int16_t innerX = x + 2;
  const int16_t innerY = y + 2;
  const int16_t innerWidth = width - 4;
  const int16_t innerHeight = height - 4;
  const int16_t outerRadius = height / 2;
  const int16_t innerRadius = innerHeight / 2;
  if (drawFrame) tft.drawRoundRect(x, y, width, height, outerRadius, frameColor);
  // Keep both layers pill-shaped. Filling the interior as a rectangle made
  // the progress track appear square at its ends on some displays.
  tft.fillRoundRect(innerX, innerY, innerWidth, innerHeight, innerRadius, backgroundColor);

  int16_t fillWidth = 0;
  int16_t fillX = innerX;
  if (indeterminate) {
    const int16_t segmentWidth = innerWidth < 18 ? innerWidth : innerWidth / 4;
    const int16_t travel = innerWidth - segmentWidth;
    fillX += travel > 0 ? (indeterminateElapsedMs / 8) % (travel + 1) : 0;
    fillWidth = segmentWidth;
  } else {
    if (value > 100) value = 100;
    fillWidth = (int16_t)((int32_t)innerWidth * value / 100);
  }
  if (fillWidth > 0) tft.fillRoundRect(fillX, innerY, fillWidth, innerHeight, innerRadius, fillColor);
}

void updateProgressBar(TFT_eSPI &tft,
                       int16_t x,
                       int16_t y,
                       int16_t width,
                       int16_t height,
                       uint8_t previousValue,
                       uint8_t value,
                       uint16_t fillColor,
                       uint16_t backgroundColor) {
  (void)previousValue;
  if (width < 5 || height < 5) return;
  if (value > 100) value = 100;
  const int16_t innerX = x + 2;
  const int16_t innerY = y + 2;
  const int16_t innerWidth = width - 4;
  const int16_t innerHeight = height - 4;
  const int16_t currentWidth = (int16_t)((int32_t)innerWidth * value / 100);
  const int16_t innerRadius = innerHeight / 2;
  // Repaint only the rounded interior, retaining the outer frame. This
  // avoids square corners when a determinate fill advances or resets.
  tft.fillRoundRect(innerX, innerY, innerWidth, innerHeight, innerRadius, backgroundColor);
  if (currentWidth > 0) {
    tft.fillRoundRect(innerX, innerY, currentWidth, innerHeight, innerRadius, fillColor);
  }
}

}  // namespace UiKit
