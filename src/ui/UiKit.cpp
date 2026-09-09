#include "UiKit.h"

#include "icons.h"

namespace UiKit {
namespace {
constexpr int16_t NavigationIconLeft = AppBarIconPadding;
constexpr int16_t NavigationIconTop = AppBarIconPadding;
constexpr int16_t TitleX = NavigationIconLeft + NavigationIconSize + AppBarIconPadding;
// Font 4's visible glyphs sit slightly high within its 26 px cell.
constexpr int16_t TitleCenterY = AppBarHeight / 2 + 1;
constexpr int16_t ListIconX = 8;
constexpr int16_t ListTextWithIconX = ListIconX + ListIconSize + ContentPadding;

uint16_t blendRgba565(uint16_t foreground, uint16_t background, uint8_t alpha) {
  if (alpha == 0) return background;
  if (alpha == 255) return foreground;

  const uint16_t inverseAlpha = 255 - alpha;
  const uint16_t red = static_cast<uint16_t>(
      ((static_cast<uint32_t>(foreground >> 11) * alpha) +
       (static_cast<uint32_t>(background >> 11) * inverseAlpha) + 127) / 255);
  const uint16_t green = static_cast<uint16_t>(
      ((static_cast<uint32_t>((foreground >> 5) & 0x3f) * alpha) +
       (static_cast<uint32_t>((background >> 5) & 0x3f) * inverseAlpha) + 127) / 255);
  const uint16_t blue = static_cast<uint16_t>(
      ((static_cast<uint32_t>(foreground & 0x1f) * alpha) +
       (static_cast<uint32_t>(background & 0x1f) * inverseAlpha) + 127) / 255);
  return static_cast<uint16_t>((red << 11) | (green << 5) | blue);
}

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
                   NavigationIcon icon,
                   AppBarAction action) {
  tft.fillRect(0, 0, width, AppBarHeight, AppBarColor);
  const int16_t iconCenterX = NavigationIconLeft + NavigationIconSize / 2;
  const int16_t iconCenterY = NavigationIconTop + NavigationIconSize / 2;
  if (icon == NavigationIcon::Back) {
    drawBackIcon(tft, iconCenterX, iconCenterY, TFT_WHITE, AppBarColor);
  } else {
    drawCloseIcon(tft, iconCenterX, iconCenterY, TFT_WHITE, AppBarColor);
  }

  const int16_t actionWidth = action == AppBarAction::None ? 0 : AppBarHeight;
  const int16_t titleWidth = width - TitleX - ContentPadding - actionWidth;
  if (titleWidth > 0 && title.length() > 0) {
    UiText::draw(tft, UiKit::ellipsize(title, titleWidth, UiText::Size::Title),
                 TitleX, TitleCenterY, TFT_WHITE, AppBarColor,
                 UiText::Size::Title, ML_DATUM);
  }
  if (action != AppBarAction::None) {
    const int16_t actionCenterX = width - AppBarHeight / 2;
    drawZoomIcon(tft, actionCenterX, iconCenterY, action == AppBarAction::ZoomOut,
                 action == AppBarAction::ZoomOut ? TFT_CYAN : TFT_WHITE, AppBarColor);
  }
}

void drawCloseIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                   uint16_t color, uint16_t backgroundColor) {
  drawAlphaIcon(tft, centerX - ic_close_width / 2, centerY - ic_close_height / 2,
                ic_close_width, ic_close_height, ic_close_alpha, color, backgroundColor);
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

void drawZoomIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  bool zoomed, uint16_t color, uint16_t backgroundColor) {
  constexpr float strokeWidth = 2.5f;
  constexpr int16_t lensOffset = 3;
  constexpr int16_t lensRadius = 7;
  tft.drawSmoothCircle(centerX - lensOffset, centerY - lensOffset, lensRadius,
                       color, backgroundColor);
  tft.drawWideLine(centerX + 2, centerY + 2, centerX + 9, centerY + 9,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX - lensOffset - 3, centerY - lensOffset,
                   centerX - lensOffset + 3, centerY - lensOffset,
                   strokeWidth, color, backgroundColor);
  if (!zoomed) {
    tft.drawWideLine(centerX - lensOffset, centerY - lensOffset - 3,
                     centerX - lensOffset, centerY - lensOffset + 3,
                     strokeWidth, color, backgroundColor);
  }
}

void drawFooterBackIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                        uint16_t color, uint16_t backgroundColor) {
  drawAlphaIcon(tft, centerX - ic_back_width / 2, centerY - ic_back_height / 2,
                ic_back_width, ic_back_height, ic_back_alpha, color, backgroundColor);
}

void drawPageArrow(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                   bool vertical, bool forward, uint16_t color,
                   uint16_t backgroundColor) {
  if (vertical) {
    const uint16_t width = forward ? ic_down_width : ic_up_width;
    const uint16_t height = forward ? ic_down_height : ic_up_height;
    const uint8_t *alpha = forward ? ic_down_alpha : ic_up_alpha;
    drawAlphaIcon(tft, centerX - width / 2, centerY - height / 2,
                  width, height, alpha, color, backgroundColor);
    return;
  }
  constexpr float strokeWidth = 3.0f;
  constexpr float tipOffset = 9.0f;
  constexpr float tailOffset = 10.0f;
  constexpr float wing = 8.0f;
  const float tipX = centerX + (forward ? tipOffset : -tipOffset);
  const float tailX = centerX - (forward ? tailOffset : -tailOffset);
  const float wingX = centerX + (forward ? 1.0f : -1.0f);
  tft.drawWideLine(tailX, centerY, tipX, centerY, strokeWidth, color, backgroundColor);
  tft.drawWideLine(tipX, centerY, wingX, centerY - wing, strokeWidth, color, backgroundColor);
  tft.drawWideLine(tipX, centerY, wingX, centerY + wing, strokeWidth, color, backgroundColor);
}

void drawPlusIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor) {
  drawAlphaIcon(tft, centerX - ic_add_width / 2, centerY - ic_add_height / 2,
                ic_add_width, ic_add_height, ic_add_alpha, color, backgroundColor);
}

void drawHomeIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor) {
  drawAlphaIcon(tft, centerX - ic_home_width / 2, centerY - ic_home_height / 2,
                ic_home_width, ic_home_height, ic_home_alpha, color, backgroundColor);
}

void drawRecentIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                    uint16_t color, uint16_t backgroundColor) {
  drawAlphaIcon(tft, centerX - ic_recent_width / 2, centerY - ic_recent_height / 2,
                ic_recent_width, ic_recent_height, ic_recent_alpha, color, backgroundColor);
}

void drawFavoriteIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                      uint16_t color, uint16_t backgroundColor) {
  drawAlphaIcon(tft, centerX - ic_star_width / 2, centerY - ic_star_height / 2,
                ic_star_width, ic_star_height, ic_star_alpha, color, backgroundColor);
}

void drawMoreIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor) {
  tft.fillSmoothCircle(centerX, centerY - 7, 2, color, backgroundColor);
  tft.fillSmoothCircle(centerX, centerY, 2, color, backgroundColor);
  tft.fillSmoothCircle(centerX, centerY + 7, 2, color, backgroundColor);
}

void drawListCloseIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                       uint16_t color, uint16_t backgroundColor) {
  drawAlphaIcon(tft, centerX - ic_close_16_width / 2, centerY - ic_close_16_height / 2,
                ic_close_16_width, ic_close_16_height, ic_close_16_alpha, color, backgroundColor);
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
  drawAlphaIcon(tft, centerX - ic_backspace_width / 2, centerY - ic_backspace_height / 2,
                ic_backspace_width, ic_backspace_height, ic_backspace_alpha, color, backgroundColor);
}

void drawKeyboardEnterIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                           uint16_t color, uint16_t backgroundColor) {
  drawAlphaIcon(tft, centerX - ic_enter_width / 2, centerY - ic_enter_height / 2,
                ic_enter_width, ic_enter_height, ic_enter_alpha, color, backgroundColor);
}

void drawKeyboardShiftIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                           bool shifted, bool capsLock,
                           uint16_t color, uint16_t backgroundColor) {
  const uint16_t width = capsLock ? ic_shift_lock_width : (shifted ? ic_shift_on_width : ic_shift_off_width);
  const uint16_t height = capsLock ? ic_shift_lock_height : (shifted ? ic_shift_on_height : ic_shift_off_height);
  const uint8_t *alpha = capsLock ? ic_shift_lock_alpha : (shifted ? ic_shift_on_alpha : ic_shift_off_alpha);
  drawAlphaIcon(tft, centerX - width / 2, centerY - height / 2,
                width, height, alpha, color, backgroundColor);
}

void drawRgba565Icon(TFT_eSPI &tft,
                     int16_t x,
                     int16_t y,
                     uint16_t width,
                     uint16_t height,
                     const uint16_t *colors,
                     const uint8_t *alpha,
                     uint16_t backgroundColor) {
  if (!colors || !alpha || width == 0 || height == 0 || width > MaxRgba565IconWidth) return;

  uint16_t row[MaxRgba565IconWidth];
  tft.startWrite();
  for (uint16_t rowIndex = 0; rowIndex < height; rowIndex++) {
    const size_t offset = static_cast<size_t>(rowIndex) * width;
    uint16_t runStart = 0;
    bool inRun = false;
    for (uint16_t column = 0; column < width; column++) {
      const uint8_t opacity = pgm_read_byte(alpha + offset + column);
      if (opacity == 0) {
        if (inRun) {
          tft.pushImage(x + runStart, y + static_cast<int16_t>(rowIndex),
                        column - runStart, 1, row + runStart);
          inRun = false;
        }
        continue;
      }
      if (!inRun) {
        runStart = column;
        inRun = true;
      }
      const uint16_t foreground = pgm_read_word(colors + offset + column);
      row[column] = blendRgba565(foreground, backgroundColor, opacity);
    }
    if (inRun) {
      tft.pushImage(x + runStart, y + static_cast<int16_t>(rowIndex),
                    width - runStart, 1, row + runStart);
    }
  }
  tft.endWrite();
}

void drawAlphaIcon(TFT_eSPI &tft,
                   int16_t x,
                   int16_t y,
                   uint16_t width,
                   uint16_t height,
                   const uint8_t *alpha,
                   uint16_t color,
                   uint16_t backgroundColor) {
  if (!alpha || width == 0 || height == 0 || width > MaxRgba565IconWidth) return;

  uint16_t row[MaxRgba565IconWidth];
  tft.startWrite();
  for (uint16_t rowIndex = 0; rowIndex < height; rowIndex++) {
    const size_t offset = static_cast<size_t>(rowIndex) * width;
    uint16_t runStart = 0;
    bool inRun = false;
    for (uint16_t column = 0; column < width; column++) {
      const uint8_t opacity = pgm_read_byte(alpha + offset + column);
      if (opacity == 0) {
        if (inRun) {
          tft.pushImage(x + runStart, y + static_cast<int16_t>(rowIndex),
                        column - runStart, 1, row + runStart);
          inRun = false;
        }
        continue;
      }
      if (!inRun) {
        runStart = column;
        inRun = true;
      }
      row[column] = blendRgba565(color, backgroundColor, opacity);
    }
    if (inRun) {
      tft.pushImage(x + runStart, y + static_cast<int16_t>(rowIndex),
                    width - runStart, 1, row + runStart);
    }
  }
  tft.endWrite();
}

void drawListCell(TFT_eSPI &tft,
                  int16_t width,
                  int16_t y,
                  const String &label,
                  const String &trailingText) {
  tft.fillRect(0, y, width, ListCellHeight, TFT_BLACK);
  const int16_t textX = ContentPadding;

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

void drawListCell(TFT_eSPI &tft,
                  int16_t width,
                  int16_t y,
                  const String &label,
                  uint16_t iconWidth,
                  uint16_t iconHeight,
                  const uint8_t *iconAlpha,
                  uint16_t iconColor,
                  const String &trailingText) {
  tft.fillRect(0, y, width, ListCellHeight, TFT_BLACK);
  if (iconAlpha && iconWidth && iconHeight) {
    drawAlphaIcon(tft, ListIconX + (ListIconSize - iconWidth) / 2,
                  y + (ListCellHeight - iconHeight) / 2,
                  iconWidth, iconHeight, iconAlpha, iconColor, TFT_BLACK);
  }

  int16_t trailingWidth = 0;
  if (trailingText.length() > 0) {
    trailingWidth = UiText::measure(trailingText, UiText::Size::Compact);
    UiText::draw(tft, trailingText, width - ContentPadding, y + ListCellHeight / 2,
                 TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact, MR_DATUM);
  }

  const int16_t labelEnd = width - ContentPadding -
                           (trailingWidth > 0 ? trailingWidth + 8 : 0);
  UiText::draw(tft, UiKit::ellipsize(label, labelEnd - ListTextWithIconX, UiText::Size::Content),
               ListTextWithIconX, y + ListCellHeight / 2, TFT_WHITE, TFT_BLACK,
               UiText::Size::Content, ML_DATUM);
  tft.drawFastHLine(ListTextWithIconX, y + ListCellHeight - 1,
                    width - ContentPadding - ListTextWithIconX, DividerColor);
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
  const int16_t innerWidth = width - 4;
  // The smooth frame's lower antialias edge leaves one extra visible pixel.
  // Keep the original top inset and extend the track by one pixel downward.
  const int16_t innerY = y + 2;
  const int16_t innerHeight = height - 3;
  const int16_t outerRadius = height / 2;
  const int16_t innerRadius = innerHeight / 2;
  if (drawFrame) {
    // Matching radii produce a one-pixel antialiased outline. A smaller
    // inner radius made the frame visibly thicker and visually displaced the
    // centered blue fill.
    tft.drawSmoothRoundRect(x, y, outerRadius, outerRadius,
                            width, height, frameColor, backgroundColor);
  }
  // Keep both layers pill-shaped. Filling the interior as a rectangle made
  // the progress track appear square at its ends on some displays.
  tft.fillSmoothRoundRect(innerX, innerY, innerWidth, innerHeight, innerRadius,
                          backgroundColor, backgroundColor);

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
  if (fillWidth > 0) {
    tft.fillSmoothRoundRect(fillX, innerY, fillWidth, innerHeight, innerRadius,
                            fillColor, backgroundColor);
  }
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
  const int16_t innerWidth = width - 4;
  const int16_t innerY = y + 2;
  const int16_t innerHeight = height - 3;
  const int16_t currentWidth = (int16_t)((int32_t)innerWidth * value / 100);
  const int16_t innerRadius = innerHeight / 2;
  // Repaint only the rounded interior, retaining the outer frame. This
  // avoids square corners when a determinate fill advances or resets.
  tft.fillSmoothRoundRect(innerX, innerY, innerWidth, innerHeight, innerRadius,
                          backgroundColor, backgroundColor);
  if (currentWidth > 0) {
    tft.fillSmoothRoundRect(innerX, innerY, currentWidth, innerHeight, innerRadius,
                            fillColor, backgroundColor);
  }
}

}  // namespace UiKit
