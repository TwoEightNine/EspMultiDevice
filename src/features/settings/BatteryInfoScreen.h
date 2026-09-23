#pragma once

#include "../../core/BatteryController.h"
#include "../../core/Screen.h"
#include "../../ui/UiKit.h"

class BatteryInfoScreen : public Screen {
public:
  BatteryInfoScreen(Navigator &navigator,
                    TouchController &touch,
                    DisplayController &display,
                    BatteryController &battery);

  void onEnter() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  struct Sample {
    BatteryReading reading;
    uint32_t sampledMs;
  };

  static const int16_t ReadingTop = UiKit::AppBarHeight + 16;
  static const uint32_t RefreshIntervalMs = 1000UL;
  static const int16_t PlotTop = 184;
  static const int16_t PlotBottom = 424;
  static const int16_t PlotLeft = 44;
  static const int16_t PlotRightGutter = 44;
  static const uint16_t MaxHistorySamples = 256;
  static const uint8_t HorizontalGridSteps = 4;
  static const uint16_t VerticalGridSeconds = 30;
  static const uint16_t TimeLabelSeconds = 60;

  BatteryController &_battery;
  BatteryReading _reading;
  bool _lastTouchDown;
  uint32_t _lastRefreshMs;
  Sample _samples[MaxHistorySamples];
  uint16_t _sampleStart;
  uint16_t _sampleCount;

  void draw();
  void drawReading();
  void drawPlot(uint32_t nowMs);
  void drawTrace(uint32_t nowMs, float scaleMinimum, float scaleMaximum, bool voltage);
  void appendSample(uint32_t nowMs);
  void discardExpiredSamples(uint32_t nowMs);
  const Sample &sampleAt(uint16_t index) const;
  int16_t plotRight() const;
  int16_t plotWidth() const;
  int16_t sampleX(const Sample &sample, uint32_t nowMs) const;
  int16_t sampleY(float value, float scaleMinimum, float scaleMaximum) const;
};
