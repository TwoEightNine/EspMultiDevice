#pragma once

#include "../../core/Screen.h"
#include "../../services/storage/BatteryHistoryService.h"
#include "../../ui/UiKit.h"

class BatteryInfoScreen : public Screen {
public:
  BatteryInfoScreen(Navigator &navigator,
                    TouchController &touch,
                    DisplayController &display,
                    BatteryHistoryService &history);

  void onEnter() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const int16_t ReadingTop = UiKit::AppBarHeight + 16;
  static const int16_t PlotTop = 170;
  static const int16_t PlotBottom = 426;
  static const int16_t PlotLeft = 44;
  static const int16_t PlotRightGutter = 16;
  static const uint8_t HorizontalGridSteps = 4;

  BatteryHistoryService &_history;
  bool _lastTouchDown;
  uint32_t _lastDrawnGeneration;

  void draw();
  void drawReading();
  void drawPlot();
  void drawTrace(float scaleMinimum, float scaleMaximum);
  int16_t plotRight() const;
  int16_t sampleX(const BatteryHistoryPoint &point, uint32_t durationMs) const;
  int16_t sampleY(float value, float scaleMinimum, float scaleMaximum) const;
};
