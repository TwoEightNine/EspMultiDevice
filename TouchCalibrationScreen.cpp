#include "TouchCalibrationScreen.h"

#include "Navigator.h"
#include "UiKit.h"

#include <string.h>

TouchCalibrationScreen::TouchCalibrationScreen(Navigator &navigator,
                                               TouchController &touch,
                                               DisplayController &display)
  : Screen(navigator, touch, display),
    _mode(Mode::Menu),
    _lastTouchDown(false),
    _calibrationWaitingForRelease(false),
    _calibrationIndex(0),
    _status("") {
  resetCalibration();
}

void TouchCalibrationScreen::onEnter() {
  _mode = Mode::Menu;
  _lastTouchDown = false;
  _calibrationWaitingForRelease = false;
  _status = "";
  draw();
}

void TouchCalibrationScreen::onExit() {
  _touch.setCalibrationCapture(false);
}

void TouchCalibrationScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    if (_mode == Mode::Calibrate) {
      _calibrationWaitingForRelease = false;
    }
    return;
  }

  if (UiKit::navigationIconContains(point.x, point.y)) {
    if (_mode == Mode::Menu) {
      _navigator.pop();
    } else {
      _touch.setCalibrationCapture(false);
      _calibrationWaitingForRelease = false;
      _mode = Mode::Menu;
      _status = "";
      draw();
    }
    return;
  }

  if (_mode == Mode::Calibrate && _calibrationWaitingForRelease) {
    drawCalibrationTouch(point, "Release first");
    return;
  }

  if (_lastTouchDown) {
    if (_mode == Mode::Calibrate) {
      drawCalibrationTouch(point, "Tracking");
    }
    return;
  }
  _lastTouchDown = true;

  if (_mode == Mode::Menu) {
    for (int16_t i = 0; i < 3; i++) {
      if (!inButton(point.x, point.y, i)) {
        continue;
      }

      if (i == 0) {
        _mode = Mode::Draw;
        _status = "";
        draw();
      } else if (i == 1) {
        _mode = Mode::Calibrate;
        resetCalibration();
        _calibrationIndex = 0;
        // Do not reuse the press that selected Calibrate as the first sample.
        // The next release arms the first target.
        _calibrationWaitingForRelease = true;
        _touch.setCalibrationCapture(true);
        draw();
      } else {
        _navigator.pop();
      }
      return;
    }
    return;
  }

  if (_mode == Mode::Draw) {
    TFT_eSPI &tft = _display.tft();
    tft.fillRect(0, HeaderHeight, _display.width(), _display.height() - HeaderHeight, TFT_BLACK);
    int16_t dotY = min((int16_t)point.y, (int16_t)(_display.height() - InfoHeight - 2));
    tft.fillCircle(point.x, dotY, 5, TFT_GREEN);
    char info[48];
    snprintf(info, sizeof(info), "x: %u   y: %u   z: %u", point.x, point.y, point.z);
    UiText::draw(tft, info, 10, _display.height() - InfoHeight / 2, TFT_WHITE, TFT_BLACK,
                 UiText::Size::Compact, ML_DATUM);
    return;
  }

  if (_mode == Mode::Calibrate) {
    acceptCalibrationPoint(point);
  }
}

void TouchCalibrationScreen::tick(uint32_t nowMs) {
  (void)nowMs;
}

void TouchCalibrationScreen::draw() {
  if (!_display.isReady()) {
    return;
  }

  _display.tft().fillScreen(TFT_BLACK);
  if (_mode == Mode::Menu) {
    drawMenu();
  } else if (_mode == Mode::Draw) {
    drawDrawMode();
  } else {
    drawCalibrationMode();
  }
}

void TouchCalibrationScreen::drawMenu() {
  UiKit::drawTopAppBar(_display.tft(), _display.width(), F("Touch screen"));
  TFT_eSPI &tft = _display.tft();

  int16_t firstY = (_display.height() - (3 * ButtonHeight + 2 * ButtonGap)) / 2;
  drawButton("Draw", (_display.width() - ButtonWidth) / 2, firstY, false);
  drawButton("Calibrate", (_display.width() - ButtonWidth) / 2,
             firstY + ButtonHeight + ButtonGap, false);
  drawButton("Exit", (_display.width() - ButtonWidth) / 2,
             firstY + 2 * (ButtonHeight + ButtonGap), false);

  if (_status.length() > 0) {
    UiText::draw(tft, _status, _display.width() / 2, _display.height() - 10,
                 TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact, BC_DATUM);
  }
}

void TouchCalibrationScreen::drawDrawMode() {
  drawHeader("Draw");
  TFT_eSPI &tft = _display.tft();
  UiText::draw(tft, F("Touch the screen to draw a point"), _display.width() / 2,
               _display.height() - InfoHeight + 14, TFT_DARKGREY, TFT_BLACK,
               UiText::Size::Compact, BC_DATUM);
}

void TouchCalibrationScreen::drawCalibrationMode() {
  drawHeader("Calibrate");
  drawCalibrationTarget();
  TFT_eSPI &tft = _display.tft();
  UiText::draw(tft, F("Touch the target"), _display.width() / 2,
               _display.height() - 10, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, BC_DATUM);
}

void TouchCalibrationScreen::drawHeader(const char *title) {
  UiKit::drawTopAppBar(_display.tft(), _display.width(), title);
}

void TouchCalibrationScreen::drawButton(const char *label,
                                        int16_t x,
                                        int16_t y,
                                        bool selected) {
  TFT_eSPI &tft = _display.tft();
  uint16_t fill = selected ? TFT_DARKGREEN : TFT_DARKGREY;
  tft.fillSmoothRoundRect(x, y, ButtonWidth, ButtonHeight, 7, fill, TFT_BLACK);
  UiText::draw(tft, label, x + ButtonWidth / 2, y + ButtonHeight / 2,
               TFT_WHITE, fill, UiText::Size::Content, MC_DATUM);
}

void TouchCalibrationScreen::drawCalibrationTarget() {
  uint8_t row = _calibrationIndex / GridSize;
  uint8_t column = _calibrationIndex % GridSize;
  int16_t x = targetX(column);
  int16_t y = targetY(row);
  TFT_eSPI &tft = _display.tft();
  tft.drawRect(max((int16_t)0, (int16_t)(x - TargetSize / 2)),
               max((int16_t)0, (int16_t)(y - TargetSize / 2)),
               TargetSize, TargetSize, TFT_YELLOW);
  tft.drawLine(max((int16_t)0, (int16_t)(x - 10)), y,
               min((int16_t)(_display.width() - 1), (int16_t)(x + 10)), y, TFT_YELLOW);
  tft.drawLine(x, max((int16_t)0, (int16_t)(y - 10)), x,
               min((int16_t)(_display.height() - InfoHeight - 1), (int16_t)(y + 10)), TFT_YELLOW);
}

void TouchCalibrationScreen::drawCalibrationTouch(const TouchPoint &point,
                                                   const char *status) {
  if (!_display.isReady()) {
    return;
  }

  TFT_eSPI &tft = _display.tft();
  int16_t dotY = min((int16_t)point.y,
                     (int16_t)(_display.height() - InfoHeight - 3));
  tft.fillCircle(point.x, dotY, 5, TFT_CYAN);

  tft.fillRect(0, _display.height() - InfoHeight,
               _display.width(), InfoHeight, TFT_BLACK);
  char info[72];
  snprintf(info, sizeof(info), "%s  x:%u y:%u z:%u",
           status, point.x, point.y, point.z);
  UiText::draw(tft, info, 8, _display.height() - InfoHeight / 2, TFT_WHITE, TFT_BLACK,
               UiText::Size::Compact, ML_DATUM);
}

void TouchCalibrationScreen::acceptCalibrationPoint(const TouchPoint &point) {
  uint8_t row = _calibrationIndex / GridSize;
  uint8_t column = _calibrationIndex % GridSize;
  _calibration.rawX[row][column] = point.rawX;
  _calibration.rawY[row][column] = point.rawY;
  _calibration.rawZ[row][column] = point.z;
  _calibrationWaitingForRelease = true;

  _calibrationIndex++;
  if (_calibrationIndex >= GridSize * GridSize) {
    bool persisted = _touch.setCalibration(_calibration, true);
    _touch.setCalibrationCapture(false);
    _mode = Mode::Menu;
    _status = persisted ? "Calibration saved" : "Calibration applied; save failed";
    draw();
    return;
  }

  draw();
  drawCalibrationTouch(point, "Sample accepted");
}

bool TouchCalibrationScreen::inButton(int16_t x, int16_t y, int16_t index) const {
  int16_t firstY = (_display.height() - (3 * ButtonHeight + 2 * ButtonGap)) / 2;
  int16_t buttonY = firstY + index * (ButtonHeight + ButtonGap);
  int16_t buttonX = (_display.width() - ButtonWidth) / 2;
  return x >= buttonX && x < buttonX + ButtonWidth &&
         y >= buttonY && y < buttonY + ButtonHeight;
}

int16_t TouchCalibrationScreen::targetX(uint8_t column) const {
  return (int32_t)column * (_display.width() - 1) / 3;
}

int16_t TouchCalibrationScreen::targetY(uint8_t row) const {
  const int16_t top = HeaderHeight + TargetSize / 2;
  const int16_t bottom = _display.height() - InfoHeight - TargetSize / 2 - 1;
  return top + (int32_t)row * (bottom - top) / 3;
}

void TouchCalibrationScreen::resetCalibration() {
  memset(&_calibration, 0, sizeof(_calibration));
}
