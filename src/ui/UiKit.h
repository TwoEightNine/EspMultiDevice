#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "UiText.h"

namespace UiKit {

// App-bar geometry. The 24px glyph is centered inside a 48px hit target.
constexpr int16_t AppBarHeight = 48;
constexpr int16_t NavigationIconSize = 24;
constexpr int16_t AppBarIconPadding = (AppBarHeight - NavigationIconSize) / 2;
// Action and keyboard glyph canvases; their owning cells supply the padding.
constexpr int16_t ListActionIconSize = 16;
constexpr int16_t ContentPadding = 16;
constexpr int16_t ListCellHeight = 40;
constexpr int16_t ListIconSize = 32;
constexpr int16_t BatteryGlyphWidth = 46;
constexpr int16_t BatteryGlyphHeight = 16;
constexpr int16_t BatteryGlyphTextWidth = 38;
// Compile-time full-colour icon rows are blended on the UI task. Keeping the
// buffer bounded avoids a heap allocation for each redraw.
constexpr uint16_t MaxRgba565IconWidth = 64;
constexpr uint16_t AppBarColor = 0x1082;  // #111111 in RGB565.
constexpr uint16_t DividerColor = TFT_DARKGREY;

struct AppBarStyle {
  uint16_t backgroundColor;
  uint16_t foregroundColor;
};

constexpr AppBarStyle DefaultAppBarStyle = {AppBarColor, TFT_WHITE};

enum class NavigationIcon : uint8_t {
  Back,
  Close
};

// Optional trailing action for a top app bar.  The action occupies the full
// 48px trailing cell, and the title is ellipsized before that cell.
enum class AppBarAction : uint8_t {
  None,
  ZoomIn,
  ZoomOut,
  More
};

// Glyph states for a circular voice-action control. The control itself owns
// its fill and complete hit target; this helper draws only the foreground.
enum class VoiceActionIcon : uint8_t {
  Microphone,
  Stop,
  Cancel,
  Unavailable
};

// Fixed glyphs used by the Mac Remote action grid. The enum order matches the
// screen's row-major 3x3 layout.
enum class RemoteActionIcon : uint8_t {
  YouTube,
  Screenshot,
  Lock,
  Previous,
  PlayPause,
  Next,
  Mute,
  VolumeDown,
  VolumeUp
};

bool navigationIconContains(int16_t x, int16_t y);
// All app-bar trailing actions occupy the complete 48px cell.
bool appBarActionContains(int16_t x, int16_t y, int16_t displayWidth);

String ellipsize(const String &text,
                 int16_t maxWidth,
                 UiText::Size size = UiText::Size::Content);

void drawTopAppBar(TFT_eSPI &tft,
                   int16_t width,
                   const String &title,
                   NavigationIcon icon = NavigationIcon::Close,
                   AppBarAction action = AppBarAction::None,
                   AppBarStyle style = DefaultAppBarStyle);

// Anti-aliased 24px navigation/control icons. Pass the colour immediately
// behind an icon so edge pixels blend correctly without display readback.
void drawCloseIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                   uint16_t color, uint16_t backgroundColor);
void drawBackIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor);
void drawZoomIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  bool zoomed, uint16_t color, uint16_t backgroundColor);
// Fullscreen and fullscreen-exit are 24px controls. Their owner provides a
// complete 48px hit target and the icon's solid immediate background.
void drawFullscreenIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                        bool exitFullscreen, uint16_t color, uint16_t backgroundColor);
// Compact playback control used by streaming media screens.
void drawPlaybackIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                      bool paused, uint16_t color, uint16_t backgroundColor);
// Compact microphone status glyph for persistent status strips. The owner
// supplies the complete hit target (if interactive) and the exact solid
// background immediately behind the antialiased primitives.
void drawMicrophoneStatusIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                              bool listening, bool available,
                              uint16_t color, uint16_t backgroundColor);
// Compact status-bar battery glyph. Voltage text is one decimal place with no
// separating space (for example, 3.7V) and is centered in the 38px body.
String batteryVoltageText(float voltage);
uint16_t batteryVoltageColor(float voltage);
void drawBatteryVoltage(TFT_eSPI &tft, int16_t x, int16_t y, float voltage,
                        uint16_t backgroundColor = TFT_BLACK);
void drawVoiceActionIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                         VoiceActionIcon icon, uint16_t color,
                         uint16_t backgroundColor);
// 40px assets used by Files and Browser footer controls. The visual canvas
// fills the 40px-tall footer cells while their complete cells remain tappable.
void drawFooterBackIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                        uint16_t color, uint16_t backgroundColor);
void drawPageArrow(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                   bool vertical, bool forward, uint16_t color,
                   uint16_t backgroundColor);
void drawPlusIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor);
void drawHomeIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor);
void drawRecentIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                    uint16_t color, uint16_t backgroundColor);
void drawFavoriteIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                      uint16_t color, uint16_t backgroundColor);
void drawMoreIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor);
void drawAppsIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                  uint16_t color, uint16_t backgroundColor);
void drawMacRemoteIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                       uint16_t color, uint16_t backgroundColor);
void drawRemoteActionIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                          RemoteActionIcon icon, uint16_t color,
                          uint16_t backgroundColor);
// Shared black popup-menu chrome. Rows use the standard 40px list-cell
// height and may show their selected state without requiring a separate icon.
void drawPopupMenuFrame(TFT_eSPI &tft, int16_t x, int16_t y,
                        int16_t width, int16_t height,
                        uint16_t backgroundColor = TFT_BLACK);
void drawPopupMenuRow(TFT_eSPI &tft, int16_t x, int16_t y,
                      int16_t width, const String &label, bool selected,
                      uint16_t backgroundColor = TFT_BLACK);
void drawListCloseIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                       uint16_t color, uint16_t backgroundColor);
void drawListMoreIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                      uint16_t color, uint16_t backgroundColor);
// Anti-aliased 16px keyboard icons. Caps Lock is indicated only when
// `capsLock` is true; Shift state colouring remains the caller's choice.
void drawKeyboardBackspaceIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                               uint16_t color, uint16_t backgroundColor);
void drawKeyboardEnterIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                           uint16_t color, uint16_t backgroundColor);
void drawKeyboardShiftIcon(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                           bool shifted, bool capsLock,
                           uint16_t color, uint16_t backgroundColor);

// Draws a flash-resident, straight-alpha RGB565 icon over a solid immediate
// background. Fully transparent pixels are left untouched. `colors` and
// `alpha` contain width * height entries generated by tools/png_to_rgba565_icon.py.
// Icons wider than MaxRgba565IconWidth are rejected so drawing remains allocation-free.
void drawRgba565Icon(TFT_eSPI &tft,
                     int16_t x,
                     int16_t y,
                     uint16_t width,
                     uint16_t height,
                     const uint16_t *colors,
                     const uint8_t *alpha,
                     uint16_t backgroundColor);

// Draws a flash-resident, straight-alpha RGB565 image of any uint16_t
// dimensions over a solid immediate background. Wide rows are processed in
// MaxRgba565IconWidth chunks so drawing remains allocation-free. Fully
// transparent pixels are left untouched.
void drawRgba565Image(TFT_eSPI &tft,
                      int16_t x,
                      int16_t y,
                      uint16_t width,
                      uint16_t height,
                      const uint16_t *colors,
                      const uint8_t *alpha,
                      uint16_t backgroundColor);

// Draws a flash-resident alpha mask with a caller-selected foreground colour.
// Fully transparent pixels are left untouched. `alpha` contains width * height
// 8-bit opacity entries. Icons wider than MaxRgba565IconWidth are rejected so
// drawing remains allocation-free.
void drawAlphaIcon(TFT_eSPI &tft,
                   int16_t x,
                   int16_t y,
                   uint16_t width,
                   uint16_t height,
                   const uint8_t *alpha,
                   uint16_t color,
                   uint16_t backgroundColor);

void drawListCell(TFT_eSPI &tft,
                  int16_t width,
                  int16_t y,
                  const String &label,
                  const String &trailingText = String());

// Draws a standard 40px list cell with a 32px alpha-mask icon. The caller
// supplies the tint so folder/file categories remain distinguishable.
void drawListCell(TFT_eSPI &tft,
                  int16_t width,
                  int16_t y,
                  const String &label,
                  uint16_t iconWidth,
                  uint16_t iconHeight,
                  const uint8_t *iconAlpha,
                  uint16_t iconColor,
                  const String &trailingText = String());

// Shared fill-only rounded text button. The caller owns hit testing for the
// complete rectangle and supplies the immediate screen background.
void drawFilledButton(TFT_eSPI &tft,
                      int16_t x,
                      int16_t y,
                      int16_t width,
                      int16_t height,
                      const String &label,
                      uint16_t fillColor,
                      uint16_t textColor = TFT_WHITE,
                      uint16_t backgroundColor = TFT_BLACK);

// Shared fill-only on/off control. The owner makes its enclosing row or
// bounds tappable; this helper only draws the visual control.
void drawToggle(TFT_eSPI &tft,
                int16_t x,
                int16_t y,
                int16_t width,
                int16_t height,
                bool enabled,
                uint16_t activeColor = TFT_BLUE,
                uint16_t backgroundColor = TFT_BLACK);

// Shared fill-only color selection button. A white marker denotes the active
// value, so swatches retain their actual color without an outlined treatment.
void drawColorSwatch(TFT_eSPI &tft,
                     int16_t x,
                     int16_t y,
                     int16_t width,
                     int16_t height,
                     const String &label,
                     uint16_t fillColor,
                     bool selected,
                     uint16_t backgroundColor = TFT_BLACK);

// Draws a compact progress bar. Call with drawFrame=false to update only the
// moving fill while retaining the existing border and avoiding visible flicker.
// When indeterminate is true, value is ignored and a moving segment is shown.
// Pass elapsed time since the operation began to start that segment at the
// left edge; the default of zero leaves it at the start position.
void drawProgressBar(TFT_eSPI &tft,
                     int16_t x,
                     int16_t y,
                     int16_t width,
                     int16_t height,
                     uint8_t value,
                     bool indeterminate = false,
                     uint16_t fillColor = TFT_BLUE,
                     uint16_t frameColor = TFT_DARKGREY,
                     uint16_t backgroundColor = TFT_WHITE,
                     bool drawFrame = true,
                     uint32_t indeterminateElapsedMs = 0);

// Updates a determinate bar without clearing its existing fill. If progress
// moves backwards, the bar is safely repainted from empty instead.
void updateProgressBar(TFT_eSPI &tft,
                       int16_t x,
                       int16_t y,
                       int16_t width,
                       int16_t height,
                       uint8_t previousValue,
                       uint8_t value,
                       uint16_t fillColor = TFT_BLUE,
                       uint16_t backgroundColor = TFT_WHITE);

}  // namespace UiKit
