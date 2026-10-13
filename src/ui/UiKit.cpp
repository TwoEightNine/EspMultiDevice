#include "UiKit.h"

#include "icons.h"

namespace UiKit {

void drawAppsIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor) {
  for (int16_t row = 0; row < 2; ++row) {
    for (int16_t column = 0; column < 2; ++column) {
      tft.fillSmoothRoundRect(centerX - 21 + column * 24,
                              centerY - 21 + row * 24, 18, 18, 4,
                              color, backgroundColor);
    }
  }
}

void drawMacRemoteIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                       uint16_t color, uint16_t backgroundColor) {
  tft.fillSmoothRoundRect(centerX - 23, centerY - 17, 46, 31, 5,
                          color, backgroundColor);
  tft.fillSmoothRoundRect(centerX - 19, centerY - 13, 38, 23, 3,
                          backgroundColor, color);
  tft.drawWideLine(centerX, centerY + 14, centerX, centerY + 21,
                   3.0f, color, backgroundColor);
  tft.drawWideLine(centerX - 10, centerY + 21, centerX + 10, centerY + 21,
                   3.0f, color, backgroundColor);
  // Three small key cells distinguish this from the shared browser/home icon.
  for (int16_t column = -1; column <= 1; ++column) {
    tft.fillSmoothRoundRect(centerX + column * 10 - 3, centerY - 4,
                            6, 6, 2, color, backgroundColor);
  }
}

void drawRemoteActionIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                          RemoteActionIcon icon, uint16_t color,
                          uint16_t backgroundColor) {
  constexpr float stroke = 3.0f;
  switch (icon) {
    case RemoteActionIcon::YouTube:
      tft.fillSmoothRoundRect(centerX - 18, centerY - 12, 36, 24, 7,
                              color, backgroundColor);
      drawPlaybackIcon(tft, centerX + 1, centerY, false,
                       backgroundColor, color);
      return;
    case RemoteActionIcon::Screenshot:
      tft.drawWideLine(centerX - 15, centerY - 7, centerX - 15, centerY - 14,
                       stroke, color, backgroundColor);
      tft.drawWideLine(centerX - 15, centerY - 14, centerX - 8, centerY - 14,
                       stroke, color, backgroundColor);
      tft.drawWideLine(centerX + 8, centerY - 14, centerX + 15, centerY - 14,
                       stroke, color, backgroundColor);
      tft.drawWideLine(centerX + 15, centerY - 14, centerX + 15, centerY - 7,
                       stroke, color, backgroundColor);
      tft.drawWideLine(centerX - 15, centerY + 7, centerX - 15, centerY + 14,
                       stroke, color, backgroundColor);
      tft.drawWideLine(centerX - 15, centerY + 14, centerX - 8, centerY + 14,
                       stroke, color, backgroundColor);
      tft.drawWideLine(centerX + 8, centerY + 14, centerX + 15, centerY + 14,
                       stroke, color, backgroundColor);
      tft.drawWideLine(centerX + 15, centerY + 14, centerX + 15, centerY + 7,
                       stroke, color, backgroundColor);
      tft.fillSmoothCircle(centerX, centerY, 5, color, backgroundColor);
      return;
    case RemoteActionIcon::Lock:
      tft.drawSmoothArc(centerX, centerY - 6, 9, 6, 180, 360,
                        color, backgroundColor, true);
      tft.fillSmoothRoundRect(centerX - 13, centerY - 3, 26, 21, 5,
                              color, backgroundColor);
      tft.fillSmoothCircle(centerX, centerY + 5, 2, backgroundColor, color);
      tft.drawWideLine(centerX, centerY + 6, centerX, centerY + 11,
                       2.0f, backgroundColor, color);
      return;
    case RemoteActionIcon::Previous:
      tft.drawWideLine(centerX - 13, centerY - 11,
                       centerX - 13, centerY + 11,
                       stroke, color, backgroundColor);
      drawPageArrow(tft, centerX + 2, centerY, false, false,
                    color, backgroundColor);
      return;
    case RemoteActionIcon::PlayPause:
      drawPlaybackIcon(tft, centerX, centerY, false, color, backgroundColor);
      return;
    case RemoteActionIcon::Next:
      tft.drawWideLine(centerX + 13, centerY - 11,
                       centerX + 13, centerY + 11,
                       stroke, color, backgroundColor);
      drawPageArrow(tft, centerX - 2, centerY, false, true,
                    color, backgroundColor);
      return;
    case RemoteActionIcon::Mute:
    case RemoteActionIcon::VolumeDown:
    case RemoteActionIcon::VolumeUp: {
      tft.fillSmoothRoundRect(centerX - 16, centerY - 5, 7, 10, 2,
                              color, backgroundColor);
      tft.drawWideLine(centerX - 9, centerY - 5, centerX - 2, centerY - 11,
                       4.0f, color, backgroundColor);
      tft.drawWideLine(centerX - 9, centerY + 5, centerX - 2, centerY + 11,
                       4.0f, color, backgroundColor);
      tft.drawWideLine(centerX - 2, centerY - 11, centerX - 2, centerY + 11,
                       4.0f, color, backgroundColor);
      if (icon == RemoteActionIcon::Mute) {
        tft.drawWideLine(centerX + 4, centerY - 9, centerX + 17, centerY + 9,
                         stroke, color, backgroundColor);
        tft.drawWideLine(centerX + 17, centerY - 9, centerX + 4, centerY + 9,
                         stroke, color, backgroundColor);
      } else {
        tft.drawWideLine(centerX + 5, centerY, centerX + 17, centerY,
                         stroke, color, backgroundColor);
        if (icon == RemoteActionIcon::VolumeUp) {
          tft.drawWideLine(centerX + 11, centerY - 6,
                           centerX + 11, centerY + 6,
                           stroke, color, backgroundColor);
        }
      }
      return;
    }
    default:
      return;
  }
}
namespace {
constexpr int16_t NavigationIconLeft = AppBarIconPadding;
constexpr int16_t NavigationIconTop = AppBarIconPadding;
constexpr int16_t TitleX = NavigationIconLeft + NavigationIconSize + AppBarIconPadding;
// Font 4's visible glyphs sit slightly high within its 26 px cell.
constexpr int16_t TitleCenterY = AppBarHeight / 2 + 1;
constexpr int16_t ListIconX = 8;
constexpr int16_t ListTextWithIconX = ListIconX + ListIconSize + ContentPadding;
constexpr uint16_t BatteryGreen = 0x2c89;
constexpr uint16_t BatteryYellow = 0xa54a;
constexpr uint16_t BatteryRed = 0xa1e5;

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

template <bool FlashResident, typename Target>
void drawBoundedRgba565Icon(Target &target,
                            int16_t x,
                            int16_t y,
                            uint16_t width,
                            uint16_t height,
                            const uint16_t *colors,
                            const uint8_t *alpha,
                            uint16_t backgroundColor) {
  uint16_t row[MaxRgba565IconWidth];
  for (uint16_t rowIndex = 0; rowIndex < height; ++rowIndex) {
    const size_t offset = static_cast<size_t>(rowIndex) * width;
    uint16_t runStart = 0;
    bool inRun = false;
    for (uint16_t column = 0; column < width; ++column) {
      const size_t sourceIndex = offset + column;
      const uint8_t opacity = FlashResident
        ? pgm_read_byte(alpha + sourceIndex) : alpha[sourceIndex];
      if (opacity == 0) {
        if (inRun) {
          target.pushImage(x + runStart, y + static_cast<int16_t>(rowIndex),
                           column - runStart, 1, row + runStart);
          inRun = false;
        }
        continue;
      }
      if (!inRun) {
        runStart = column;
        inRun = true;
      }
      const uint16_t foreground = FlashResident
        ? pgm_read_word(colors + sourceIndex) : colors[sourceIndex];
      row[column] = blendRgba565(foreground, backgroundColor, opacity);
    }
    if (inRun) {
      target.pushImage(x + runStart, y + static_cast<int16_t>(rowIndex),
                       width - runStart, 1, row + runStart);
    }
  }
}

}  // namespace

bool navigationIconContains(int16_t x, int16_t y) {
  return x >= 0 && x < AppBarHeight && y >= 0 && y < AppBarHeight;
}

bool appBarActionContains(int16_t x, int16_t y, int16_t displayWidth) {
  return x >= displayWidth - AppBarHeight && x < displayWidth &&
         y >= 0 && y < AppBarHeight;
}

String ellipsize(const String &text, int16_t maxWidth, UiText::Size size) {
  return UiText::ellipsize(text, maxWidth, size);
}

void drawTopAppBar(TFT_eSPI &tft,
                   int16_t width,
                   const String &title,
                   NavigationIcon icon,
                   AppBarAction action,
                   AppBarStyle style) {
  tft.fillRect(0, 0, width, AppBarHeight, style.backgroundColor);
  const int16_t iconCenterX = NavigationIconLeft + NavigationIconSize / 2;
  const int16_t iconCenterY = NavigationIconTop + NavigationIconSize / 2;
  if (icon == NavigationIcon::Back) {
    drawBackIcon(tft, iconCenterX, iconCenterY, style.foregroundColor,
                 style.backgroundColor);
  } else {
    drawCloseIcon(tft, iconCenterX, iconCenterY, style.foregroundColor,
                  style.backgroundColor);
  }

  const int16_t actionWidth = action == AppBarAction::None ? 0 : AppBarHeight;
  const int16_t titleWidth = width - TitleX - ContentPadding - actionWidth;
  if (titleWidth > 0 && title.length() > 0) {
    UiText::draw(tft, UiKit::ellipsize(title, titleWidth, UiText::Size::Title),
                 TitleX, TitleCenterY, style.foregroundColor, style.backgroundColor,
                 UiText::Size::Title, ML_DATUM);
  }
  if (action != AppBarAction::None) {
    const int16_t actionCenterX = width - AppBarHeight / 2;
    if (action == AppBarAction::More) {
      drawMoreIcon(tft, actionCenterX, iconCenterY, style.foregroundColor,
                   style.backgroundColor);
    } else {
      drawZoomIcon(tft, actionCenterX, iconCenterY,
                   action == AppBarAction::ZoomOut,
                   action == AppBarAction::ZoomOut ? TFT_CYAN : style.foregroundColor,
                   style.backgroundColor);
    }
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

void drawFullscreenIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                        bool exitFullscreen, uint16_t color, uint16_t backgroundColor) {
  constexpr float strokeWidth = 2.5f;
  constexpr int16_t outer = 9;
  constexpr int16_t inner = 3;
  if (!exitFullscreen) {
    tft.drawWideLine(centerX - outer, centerY - inner, centerX - outer, centerY - outer,
                     strokeWidth, color, backgroundColor);
    tft.drawWideLine(centerX - outer, centerY - outer, centerX - inner, centerY - outer,
                     strokeWidth, color, backgroundColor);
    tft.drawWideLine(centerX + inner, centerY - outer, centerX + outer, centerY - outer,
                     strokeWidth, color, backgroundColor);
    tft.drawWideLine(centerX + outer, centerY - outer, centerX + outer, centerY - inner,
                     strokeWidth, color, backgroundColor);
    tft.drawWideLine(centerX - outer, centerY + inner, centerX - outer, centerY + outer,
                     strokeWidth, color, backgroundColor);
    tft.drawWideLine(centerX - outer, centerY + outer, centerX - inner, centerY + outer,
                     strokeWidth, color, backgroundColor);
    tft.drawWideLine(centerX + inner, centerY + outer, centerX + outer, centerY + outer,
                     strokeWidth, color, backgroundColor);
    tft.drawWideLine(centerX + outer, centerY + outer, centerX + outer, centerY + inner,
                     strokeWidth, color, backgroundColor);
    return;
  }
  tft.drawWideLine(centerX - outer, centerY - inner, centerX - inner, centerY - inner,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX - inner, centerY - inner, centerX - inner, centerY - outer,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX + inner, centerY - outer, centerX + inner, centerY - inner,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX + inner, centerY - inner, centerX + outer, centerY - inner,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX - outer, centerY + inner, centerX - inner, centerY + inner,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX - inner, centerY + inner, centerX - inner, centerY + outer,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX + inner, centerY + outer, centerX + inner, centerY + inner,
                   strokeWidth, color, backgroundColor);
  tft.drawWideLine(centerX + inner, centerY + inner, centerX + outer, centerY + inner,
                   strokeWidth, color, backgroundColor);
}

void drawPlaybackIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                      bool paused, uint16_t color, uint16_t backgroundColor) {
  if (paused) {
    tft.fillSmoothRoundRect(centerX - 6, centerY - 7, 4, 14, 2, color, backgroundColor);
    tft.fillSmoothRoundRect(centerX + 2, centerY - 7, 4, 14, 2, color, backgroundColor);
    return;
  }
  // Three antialiased strokes make a compact filled play chevron without a
  // pixelated triangle edge.
  tft.drawWideLine(centerX - 4, centerY - 7, centerX - 4, centerY + 7, 3.0f, color, backgroundColor);
  tft.drawWideLine(centerX - 3, centerY - 6, centerX + 7, centerY, 3.0f, color, backgroundColor);
  tft.drawWideLine(centerX - 3, centerY + 6, centerX + 7, centerY, 3.0f, color, backgroundColor);
}

void drawMicrophoneStatusIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                              bool listening, bool available,
                              uint16_t color, uint16_t backgroundColor) {
  const uint16_t bodyColor = available ? color : TFT_DARKGREY;
  tft.fillSmoothRoundRect(centerX - 4, centerY - 8, 8, 13, 4,
                          bodyColor, backgroundColor);
  tft.drawWideLine(centerX - 7, centerY + 1, centerX - 7, centerY + 3,
                   2.0f, bodyColor, backgroundColor);
  tft.drawWideLine(centerX + 7, centerY + 1, centerX + 7, centerY + 3,
                   2.0f, bodyColor, backgroundColor);
  tft.drawSmoothArc(centerX, centerY + 1, 8, 6, 0, 180,
                    bodyColor, backgroundColor, true);
  tft.drawWideLine(centerX, centerY + 8, centerX, centerY + 11,
                   2.0f, bodyColor, backgroundColor);
  tft.drawWideLine(centerX - 4, centerY + 11, centerX + 4, centerY + 11,
                   2.0f, bodyColor, backgroundColor);
  if (!available || !listening) {
    const uint16_t slashColor = available ? color : TFT_LIGHTGREY;
    tft.drawWideLine(centerX - 9, centerY - 9, centerX + 9, centerY + 10,
                     2.5f, slashColor, backgroundColor);
  }
}

String batteryVoltageText(float voltage) {
  String text(voltage, 1);
  text += 'V';
  return text;
}

uint16_t batteryVoltageColor(float voltage) {
  if (voltage > 3.66f) return BatteryGreen;
  if (voltage < 3.33f) return BatteryRed;
  return BatteryYellow;
}

void drawBatteryVoltage(TFT_eSPI &tft, int16_t x, int16_t y, float voltage,
                        uint16_t backgroundColor) {
  const uint16_t fill = batteryVoltageColor(voltage);
  const String text = batteryVoltageText(voltage);

  tft.fillRect(x, y, BatteryGlyphWidth, BatteryGlyphHeight, backgroundColor);
  tft.fillSmoothRoundRect(x, y, 40, BatteryGlyphHeight, 3, TFT_WHITE,
                          backgroundColor);
  tft.fillSmoothRoundRect(x + 1, y + 1, BatteryGlyphTextWidth,
                          BatteryGlyphHeight - 2, 2, fill, TFT_WHITE);
  tft.fillSmoothRoundRect(x + 40, y + 5, 3, 6, 1, TFT_WHITE,
                          backgroundColor);
  UiText::draw(tft, text, x + 20, y + BatteryGlyphHeight / 2,
               TFT_WHITE, fill, UiText::Size::Compact, MC_DATUM);
}

void drawVoiceActionIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                         VoiceActionIcon icon, uint16_t color,
                         uint16_t backgroundColor) {
  switch (icon) {
    case VoiceActionIcon::Stop:
      tft.fillSmoothRoundRect(centerX - 9, centerY - 9, 18, 18, 3,
                              color, backgroundColor);
      return;
    case VoiceActionIcon::Cancel:
      drawCloseIcon(tft, centerX, centerY, color, backgroundColor);
      return;
    case VoiceActionIcon::Unavailable:
      drawMicrophoneStatusIcon(tft, centerX, centerY, false, false,
                               color, backgroundColor);
      return;
    case VoiceActionIcon::Microphone:
    default:
      // `listening` is true here because the action glyph is an available
      // microphone, not the removed persistent status indicator.
      drawMicrophoneStatusIcon(tft, centerX, centerY, true, true,
                               color, backgroundColor);
      return;
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

void drawPopupMenuFrame(TFT_eSPI &tft, int16_t x, int16_t y,
                        int16_t width, int16_t height,
                        uint16_t backgroundColor) {
  if (width <= 0 || height <= 0) return;
  // Establish a solid immediate background before antialiasing the rounded
  // perimeter; popup content deliberately obscures anything below it.
  tft.fillRect(x - 1, y - 1, width + 2, height + 2, backgroundColor);
  tft.fillSmoothRoundRect(x, y, width, height, 6, TFT_BLACK,
                          backgroundColor);
  tft.drawSmoothRoundRect(x, y, 6, 6, width, height, TFT_DARKGREY,
                          TFT_BLACK);
}

void drawPopupMenuRow(TFT_eSPI &tft, int16_t x, int16_t y,
                      int16_t width, const String &label, bool selected,
                      uint16_t backgroundColor) {
  if (width <= 0) return;
  const uint16_t rowColor = selected ? 0x18E3 : TFT_BLACK;
  tft.fillRect(x + 1, y, width - 2, ListCellHeight, rowColor);
  UiText::draw(tft, UiKit::ellipsize(label, width - ContentPadding * 2 - 56,
                                     UiText::Size::Content),
               x + ContentPadding, y + ListCellHeight / 2, TFT_WHITE,
               rowColor, UiText::Size::Content, ML_DATUM);
  if (selected) {
    UiText::draw(tft, F("Current"), x + width - ContentPadding,
                 y + ListCellHeight / 2, TFT_CYAN, rowColor,
                 UiText::Size::Compact, MR_DATUM);
  }
  if (y > 0) {
    tft.drawFastHLine(x + ContentPadding, y, width - ContentPadding * 2,
                      selected ? rowColor : backgroundColor);
  }
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

  drawRgba565Image(tft, x, y, width, height, colors, alpha, backgroundColor);
}

void drawRgba565Icon(TFT_eSprite &sprite,
                     int16_t x,
                     int16_t y,
                     uint16_t width,
                     uint16_t height,
                     const uint16_t *colors,
                     const uint8_t *alpha,
                     uint16_t backgroundColor) {
  if (!colors || !alpha || width == 0 || height == 0 ||
      width > MaxRgba565IconWidth) return;
  const bool previousSwap = sprite.getSwapBytes();
  sprite.setSwapBytes(true);
  drawBoundedRgba565Icon<true>(sprite, x, y, width, height,
                               colors, alpha, backgroundColor);
  sprite.setSwapBytes(previousSwap);
}

void drawRgba565IconFromMemory(TFT_eSPI &tft,
                              int16_t x,
                              int16_t y,
                              uint16_t width,
                              uint16_t height,
                              const uint16_t *colors,
                              const uint8_t *alpha,
                              uint16_t backgroundColor) {
  if (!colors || !alpha || width == 0 || height == 0 ||
      width > MaxRgba565IconWidth) return;
  tft.startWrite();
  drawBoundedRgba565Icon<false>(tft, x, y, width, height,
                                colors, alpha, backgroundColor);
  tft.endWrite();
}

void drawRgba565IconFromMemory(TFT_eSprite &sprite,
                               int16_t x,
                               int16_t y,
                               uint16_t width,
                               uint16_t height,
                               const uint16_t *colors,
                               const uint8_t *alpha,
                               uint16_t backgroundColor) {
  if (!colors || !alpha || width == 0 || height == 0 ||
      width > MaxRgba565IconWidth) return;
  const bool previousSwap = sprite.getSwapBytes();
  sprite.setSwapBytes(true);
  drawBoundedRgba565Icon<false>(sprite, x, y, width, height,
                                colors, alpha, backgroundColor);
  sprite.setSwapBytes(previousSwap);
}

void drawRgba565Image(TFT_eSPI &tft,
                      int16_t x,
                      int16_t y,
                      uint16_t width,
                      uint16_t height,
                      const uint16_t *colors,
                      const uint8_t *alpha,
                      uint16_t backgroundColor) {
  if (!colors || !alpha || width == 0 || height == 0) return;

  uint16_t row[MaxRgba565IconWidth];
  tft.startWrite();
  for (uint16_t rowIndex = 0; rowIndex < height; rowIndex++) {
    const size_t offset = static_cast<size_t>(rowIndex) * width;
    for (uint16_t chunkStart = 0; chunkStart < width;) {
      const uint16_t remaining = width - chunkStart;
      const uint16_t chunkWidth = remaining < MaxRgba565IconWidth
                                    ? remaining : MaxRgba565IconWidth;
      uint16_t runStart = 0;
      bool inRun = false;
      for (uint16_t column = 0; column < chunkWidth; column++) {
        const size_t sourceIndex = offset + chunkStart + column;
        const uint8_t opacity = pgm_read_byte(alpha + sourceIndex);
        if (opacity == 0) {
          if (inRun) {
            tft.pushImage(x + chunkStart + runStart,
                          y + static_cast<int16_t>(rowIndex),
                          column - runStart, 1, row + runStart);
            inRun = false;
          }
          continue;
        }
        if (!inRun) {
          runStart = column;
          inRun = true;
        }
        const uint16_t foreground = pgm_read_word(colors + sourceIndex);
        row[column] = blendRgba565(foreground, backgroundColor, opacity);
      }
      if (inRun) {
        tft.pushImage(x + chunkStart + runStart,
                      y + static_cast<int16_t>(rowIndex),
                      chunkWidth - runStart, 1, row + runStart);
      }
      chunkStart += chunkWidth;
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

void drawFileListCell(TFT_eSPI &tft,
                      int16_t width,
                      int16_t y,
                      const String &label,
                      FileIcon icon,
                      ListAction action) {
  // Blank trailing text reserves the action cell so the label ellipsizes
  // before it, matching the Files screen.
  const String reserved = action == ListAction::None ? String() : String(F("            "));
  if (icon == FileIcon::None) {
    drawListCell(tft, width, y, label, reserved);
  } else {
    const bool folder = icon == FileIcon::Folder;
    const uint16_t color = folder ? FolderIconColor
                         : icon == FileIcon::Image ? ImageFileIconColor
                         : icon == FileIcon::Audio ? AudioFileIconColor
                         : icon == FileIcon::Video ? VideoFileIconColor
                         : icon == FileIcon::Text ? TextFileIconColor : FileIconColor;
    drawListCell(tft, width, y, label,
                 folder ? ic_folder_width : ic_file_width,
                 folder ? ic_folder_height : ic_file_height,
                 folder ? ic_folder_alpha : ic_file_alpha, color, reserved);
  }
  const int16_t centerX = width - ListCellHeight / 2;
  const int16_t centerY = y + ListCellHeight / 2;
  if (action == ListAction::More) drawListMoreIcon(tft, centerX, centerY, TFT_LIGHTGREY, TFT_BLACK);
  else if (action == ListAction::Close) drawListCloseIcon(tft, centerX, centerY, TFT_LIGHTGREY, TFT_BLACK);
}

void drawFilledButton(TFT_eSPI &tft,
                      int16_t x,
                      int16_t y,
                      int16_t width,
                      int16_t height,
                      const String &label,
                      uint16_t fillColor,
                      uint16_t textColor,
                      uint16_t backgroundColor) {
  if (width <= 0 || height <= 0) return;
  const int16_t radius = min<int16_t>(8, height / 2);
  tft.fillSmoothRoundRect(x, y, width, height, radius,
                          fillColor, backgroundColor);
  UiText::draw(tft, UiKit::ellipsize(label, width - ContentPadding * 2,
                                     UiText::Size::Content),
               x + width / 2, y + height / 2,
               textColor, fillColor, UiText::Size::Content, MC_DATUM);
}

void drawToggle(TFT_eSPI &tft,
                int16_t x,
                int16_t y,
                int16_t width,
                int16_t height,
                bool enabled,
                uint16_t activeColor,
                uint16_t backgroundColor) {
  if (width < 2 || height < 2) return;
  const int16_t radius = height / 2;
  const uint16_t fill = enabled ? activeColor : TFT_DARKGREY;
  tft.fillSmoothRoundRect(x, y, width, height, radius, fill, backgroundColor);
  const int16_t knobRadius = max<int16_t>(2, height / 2 - 6);
  const int16_t knobX = enabled ? x + width - height / 2 : x + height / 2;
  tft.fillSmoothCircle(knobX, y + height / 2, knobRadius, TFT_WHITE, fill);
}

void drawColorSwatch(TFT_eSPI &tft,
                     int16_t x,
                     int16_t y,
                     int16_t width,
                     int16_t height,
                     const String &label,
                     uint16_t fillColor,
                     bool selected,
                     uint16_t backgroundColor) {
  if (width <= 0 || height <= 0) return;
  const int16_t radius = min<int16_t>(8, height / 2);
  tft.fillSmoothRoundRect(x, y, width, height, radius, fillColor, backgroundColor);
  const uint16_t textColor = fillColor == TFT_GREEN || fillColor == TFT_YELLOW || fillColor == TFT_CYAN
                               ? TFT_BLACK : TFT_WHITE;
  UiText::draw(tft, UiKit::ellipsize(label, width - ContentPadding * 2,
                                     UiText::Size::Content),
               x + width / 2, y + height / 2, textColor, fillColor,
               UiText::Size::Content, MC_DATUM);
  if (selected) {
    tft.fillSmoothCircle(x + width - 14, y + 14, 5, TFT_WHITE, fillColor);
  }
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
