#pragma once

#include <Arduino.h>

#include "src/core/Screen.h"
#include "UiKit.h"

class TouchCalibrationScreen : public Screen {
public:
  TouchCalibrationScreen(Navigator &navigator,
                         TouchController &touch,
                         DisplayController &display);

  void onEnter() override;
  void onExit() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  enum class Mode : uint8_t {
    Menu,
    Draw,
    Calibrate
  };

  static const uint8_t GridSize = 4;
  static const int16_t HeaderHeight = UiKit::AppBarHeight;
  static const int16_t InfoHeight = 46;
  static const int16_t TargetSize = 28;
  static const int16_t ButtonWidth = 210;
  static const int16_t ButtonHeight = 48;
  static const int16_t ButtonGap = 14;

  Mode _mode;
  bool _lastTouchDown;
  bool _calibrationWaitingForRelease;
  uint8_t _calibrationIndex;
  TouchCalibrationGrid _calibration;
  String _status;

  void draw();
  void drawMenu();
  void drawDrawMode();
  void drawCalibrationMode();
  void drawHeader(const char *title);
  void drawButton(const char *label, int16_t x, int16_t y, bool selected);
  void drawCalibrationTarget();
  void drawCalibrationTouch(const TouchPoint &point, const char *status);
  void acceptCalibrationPoint(const TouchPoint &point);
  bool inButton(int16_t x, int16_t y, int16_t index) const;
  int16_t targetX(uint8_t column) const;
  int16_t targetY(uint8_t row) const;
  void resetCalibration();
};
