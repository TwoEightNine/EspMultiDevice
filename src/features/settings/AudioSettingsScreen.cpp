#include "AudioSettingsScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"

AudioSettingsScreen::AudioSettingsScreen(Navigator &navigator,
                                         TouchController &touch,
                                         DisplayController &display,
                                         AudioSettings &audioSettings)
  : Screen(navigator, touch, display),
    _audioSettings(audioSettings),
    _lastTouchDown(false) {
}

void AudioSettingsScreen::onEnter() {
  _lastTouchDown = false;
  draw();
}

void AudioSettingsScreen::handleTouch(const TouchPoint &point) {
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

  int8_t level = levelAt(point.x, point.y);
  if (level >= AudioSettings::MinVolume) {
    _audioSettings.setVolume((uint8_t)level);
    drawLevels();
  }
}

void AudioSettingsScreen::draw() {
  if (!_display.isReady()) return;

  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Audio"));
  UiText::draw(tft, F("Output volume"), _display.width() / 2, 82, TFT_WHITE, TFT_BLACK,
               UiText::Size::Title, MC_DATUM);
  drawLevels();
}

void AudioSettingsScreen::drawLevels() {
  if (!_display.isReady()) return;

  TFT_eSPI &tft = _display.tft();
  int16_t availableWidth = _display.width() - GridLeft * 2 - GridGap * (Columns - 1);
  int16_t cellWidth = availableWidth / Columns;
  uint8_t selected = _audioSettings.volume();

  for (uint8_t level = 0; level < LevelCount; level++) {
    uint8_t column = level % Columns;
    uint8_t row = level / Columns;
    int16_t x = GridLeft + column * (cellWidth + GridGap);
    int16_t y = GridTop + row * (GridHeight + GridGap);
    bool isSelected = level == selected;
    uint16_t fill = isSelected ? TFT_CYAN : TFT_DARKGREY;
    uint16_t text = isSelected ? TFT_BLACK : TFT_WHITE;
    tft.fillSmoothRoundRect(x, y, cellWidth, GridHeight, 6, fill, TFT_BLACK);
    UiText::draw(tft, String(level), x + cellWidth / 2, y + GridHeight / 2,
                 text, fill, UiText::Size::Content, MC_DATUM);
  }

  tft.fillRect(0, 252, _display.width(), 28, TFT_BLACK);
  String selectedText = F("Level ");
  selectedText += selected;
  selectedText += F(" of 10");
  UiText::draw(tft, selectedText, _display.width() / 2, 266, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
}

int8_t AudioSettingsScreen::levelAt(int16_t x, int16_t y) const {
  int16_t availableWidth = _display.width() - GridLeft * 2 - GridGap * (Columns - 1);
  int16_t cellWidth = availableWidth / Columns;
  for (uint8_t level = 0; level < LevelCount; level++) {
    uint8_t column = level % Columns;
    uint8_t row = level / Columns;
    int16_t left = GridLeft + column * (cellWidth + GridGap);
    int16_t top = GridTop + row * (GridHeight + GridGap);
    if (x >= left && x < left + cellWidth && y >= top && y < top + GridHeight) {
      return (int8_t)level;
    }
  }
  return -1;
}
