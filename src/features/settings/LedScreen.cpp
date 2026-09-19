#include "LedScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"

namespace {
constexpr int16_t ToggleWidth = 72;
constexpr int16_t ToggleHeight = 34;
constexpr int16_t PaletteTop = 142;
constexpr int16_t PaletteGap = 12;
constexpr int16_t PaletteHeight = 72;
}

LedScreen::LedScreen(Navigator &navigator,
                     TouchController &touch,
                     DisplayController &display,
                     LedSettings &ledSettings)
  : Screen(navigator, touch, display),
    _ledSettings(ledSettings),
    _lastTouchDown(false),
    _toggleBounds{0, 0, 0, 0},
    _swatchBounds{},
    _error() {
}

void LedScreen::onEnter() {
  _lastTouchDown = false;
  _error = _ledSettings.error();
  draw();
}

void LedScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

  if (UiKit::navigationIconContains(point.x, point.y)) {
    _navigator.pop();
    return;
  }

  const Rect enabledRow = {0, EnabledRowY, static_cast<int16_t>(_display.width()),
                           UiKit::ListCellHeight};
  if (contains(enabledRow, point.x, point.y)) {
    _ledSettings.setEnabled(!_ledSettings.isEnabled());
    _error = _ledSettings.error();
    draw();
    return;
  }

  if (!_ledSettings.isAvailable()) return;

  for (uint8_t index = 0; index < LedSettings::ColorCount; index++) {
    if (contains(_swatchBounds[index], point.x, point.y)) {
      _ledSettings.setColor(static_cast<LedColor>(index));
      _error = _ledSettings.error();
      draw();
      return;
    }
  }
}

void LedScreen::draw() {
  if (!_display.isReady()) return;

  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("LED"));
  drawEnabledRow();
  drawPalette();

  if (_error.length() > 0) {
    UiText::draw(tft, _error, _display.width() / 2, 336,
                 TFT_RED, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
  }
}

void LedScreen::drawEnabledRow() {
  TFT_eSPI &tft = _display.tft();
  _toggleBounds = {static_cast<int16_t>(_display.width() - UiKit::ContentPadding - ToggleWidth),
                   (int16_t)(EnabledRowY + (UiKit::ListCellHeight - ToggleHeight) / 2),
                   ToggleWidth, ToggleHeight};
  UiKit::drawListCell(tft, _display.width(), EnabledRowY, F("Enabled"));
  UiKit::drawToggle(tft, _toggleBounds.x, _toggleBounds.y,
                    _toggleBounds.w, _toggleBounds.h,
                    _ledSettings.isEnabled(), TFT_BLUE, TFT_BLACK);
}

void LedScreen::drawPalette() {
  TFT_eSPI &tft = _display.tft();
  UiText::draw(tft, F("Color"), UiKit::ContentPadding, 112,
               TFT_WHITE, TFT_BLACK, UiText::Size::Compact);

  const int16_t availableWidth = _display.width() - UiKit::ContentPadding * 2 -
                                 PaletteGap * (PaletteColumns - 1);
  const int16_t swatchWidth = availableWidth / PaletteColumns;
  for (uint8_t index = 0; index < LedSettings::ColorCount; index++) {
    const uint8_t column = index % PaletteColumns;
    const uint8_t row = index / PaletteColumns;
    const Rect bounds = {
      (int16_t)(UiKit::ContentPadding + column * (swatchWidth + PaletteGap)),
      (int16_t)(PaletteTop + row * (PaletteHeight + PaletteGap)),
      swatchWidth,
      PaletteHeight
    };
    _swatchBounds[index] = bounds;
    const LedColor color = static_cast<LedColor>(index);
    UiKit::drawColorSwatch(tft, bounds.x, bounds.y, bounds.w, bounds.h,
                           LedSettings::colorName(color), LedSettings::color565(color),
                           color == _ledSettings.color(), TFT_BLACK);
  }
}

bool LedScreen::contains(const Rect &bounds, int16_t x, int16_t y) const {
  return x >= bounds.x && x < bounds.x + bounds.w &&
         y >= bounds.y && y < bounds.y + bounds.h;
}
