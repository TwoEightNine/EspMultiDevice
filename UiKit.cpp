#include "UiKit.h"

namespace UiKit {
namespace {
constexpr int16_t NavigationIconTop = (AppBarHeight - NavigationIconSize) / 2;
constexpr int16_t TitleX = NavigationIconSize + ContentPadding;
// Font 4's visible glyphs sit slightly high within its 26 px cell.
constexpr int16_t TitleCenterY = AppBarHeight / 2 + 3;
constexpr int16_t ListIconX = 8;
constexpr int16_t ListIconY = (ListCellHeight - ListIconSize) / 2;
constexpr int16_t ListTextWithIconX = ListIconX + ListIconSize + ContentPadding;

void resetTextState(TFT_eSPI &tft) {
  tft.setFreeFont();
  tft.setTextDatum(TL_DATUM);
}

void drawBackIcon(TFT_eSPI &tft) {
  const int16_t centerY = AppBarHeight / 2;
  const int16_t left = 11;
  const int16_t point = 27;
  const int16_t right = 33;
  for (int8_t offset = -1; offset <= 1; offset++) {
    tft.drawLine(point, centerY - 10 + offset, left, centerY + offset, TFT_WHITE);
    tft.drawLine(left, centerY + offset, point, centerY + 10 + offset, TFT_WHITE);
    tft.drawLine(left, centerY + offset, right, centerY + offset, TFT_WHITE);
  }
}

void drawCloseIcon(TFT_eSPI &tft) {
  const int16_t inset = 12;
  const int16_t end = NavigationIconSize - inset;
  for (int8_t offset = -1; offset <= 1; offset++) {
    tft.drawLine(inset + offset, inset, end + offset, end, TFT_WHITE);
    tft.drawLine(end + offset, inset, inset + offset, end, TFT_WHITE);
  }
}
}  // namespace

bool navigationIconContains(int16_t x, int16_t y) {
  // Treat the entire leading app-bar corner as navigation: the 40 px icon,
  // its surrounding space, and the margin before the title.
  return x >= 0 && x < TitleX && y >= 0 && y < AppBarHeight;
}

String ellipsize(TFT_eSPI &tft, const String &text, int16_t maxWidth, uint8_t font) {
  if (maxWidth <= 0) return String();
  if (tft.textWidth(text, font) <= maxWidth) return text;

  String result = text;
  while (result.length() > 3 && tft.textWidth(result + "...", font) > maxWidth) {
    result.remove(result.length() - 1);
  }
  return result.length() == text.length() ? result : result + "...";
}

void drawTopAppBar(TFT_eSPI &tft,
                   int16_t width,
                   const String &title,
                   NavigationIcon icon) {
  tft.fillRect(0, 0, width, AppBarHeight, AppBarColor);
  if (icon == NavigationIcon::Back) drawBackIcon(tft);
  else drawCloseIcon(tft);

  const int16_t titleWidth = width - TitleX - ContentPadding;
  if (titleWidth > 0 && title.length() > 0) {
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, AppBarColor);
    tft.drawString(ellipsize(tft, title, titleWidth, TitleFont),
                   TitleX, TitleCenterY, TitleFont);
  }
  resetTextState(tft);
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
    trailingWidth = tft.textWidth(trailingText, 1);
    tft.setTextDatum(MR_DATUM);
    tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tft.drawString(trailingText, width - ContentPadding, y + ListCellHeight / 2, 1);
  }

  const int16_t labelEnd = width - ContentPadding -
                           (trailingWidth > 0 ? trailingWidth + 8 : 0);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(ellipsize(tft, label, labelEnd - textX, ListFont),
                 textX, y + ListCellHeight / 2, ListFont);
  tft.drawFastHLine(textX, y + ListCellHeight - 1,
                    width - ContentPadding - textX, DividerColor);
  resetTextState(tft);
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
  if (drawFrame) tft.drawRoundRect(x, y, width, height, 4, frameColor);
  tft.fillRect(innerX, innerY, innerWidth, innerHeight, backgroundColor);

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
  if (fillWidth > 0) tft.fillRoundRect(fillX, innerY, fillWidth, innerHeight, 3, fillColor);
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
  if (width < 5 || height < 5) return;
  if (previousValue > 100) previousValue = 100;
  if (value > 100) value = 100;
  const int16_t innerX = x + 2;
  const int16_t innerY = y + 2;
  const int16_t innerWidth = width - 4;
  const int16_t innerHeight = height - 4;
  const int16_t previousWidth = (int16_t)((int32_t)innerWidth * previousValue / 100);
  const int16_t currentWidth = (int16_t)((int32_t)innerWidth * value / 100);
  if (currentWidth < previousWidth) {
    tft.fillRect(innerX, innerY, innerWidth, innerHeight, backgroundColor);
    if (currentWidth > 0) tft.fillRect(innerX, innerY, currentWidth, innerHeight, fillColor);
    return;
  }
  if (currentWidth > previousWidth) {
    tft.fillRect(innerX + previousWidth, innerY, currentWidth - previousWidth, innerHeight, fillColor);
  }
}

}  // namespace UiKit
