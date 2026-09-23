#include "BatteryInfoScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"

namespace {
constexpr uint16_t VoltageColor = TFT_GREEN;
constexpr uint16_t PercentageColor = TFT_RED;
constexpr uint16_t AxisColor = TFT_DARKGREY;
constexpr float GridPaddingFraction = 0.1f;
constexpr float MinimumVoltageRange = 0.05f;
constexpr float MinimumPercentageRange = 4.0f;

struct PlotRange {
  float minimum;
  float maximum;
};

PlotRange paddedRange(float observedMinimum, float observedMaximum, float minimumRange) {
  if (observedMaximum - observedMinimum < minimumRange) {
    const float center = (observedMinimum + observedMaximum) * 0.5f;
    observedMinimum = center - minimumRange * 0.5f;
    observedMaximum = center + minimumRange * 0.5f;
  }

  const float range = observedMaximum - observedMinimum;
  const float padding = range * GridPaddingFraction;
  return {observedMinimum - padding, observedMaximum + padding};
}
}

BatteryInfoScreen::BatteryInfoScreen(Navigator &navigator,
                                     TouchController &touch,
                                     DisplayController &display,
                                     BatteryController &battery)
  : Screen(navigator, touch, display),
    _battery(battery),
    _reading{0.0f, 0},
    _lastTouchDown(false),
    _lastRefreshMs(0),
    _samples{},
    _sampleStart(0),
    _sampleCount(0) {
}

void BatteryInfoScreen::onEnter() {
  _lastTouchDown = false;
  _reading = _battery.readLive();
  _lastRefreshMs = millis();
  _sampleStart = 0;
  _sampleCount = 0;
  appendSample(_lastRefreshMs);
  draw();
}

void BatteryInfoScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

  if (UiKit::navigationIconContains(point.x, point.y)) {
    _navigator.pop();
  }
}

void BatteryInfoScreen::tick(uint32_t nowMs) {
  if (nowMs - _lastRefreshMs < RefreshIntervalMs) return;

  _reading = _battery.readLive();
  _lastRefreshMs = nowMs;
  appendSample(nowMs);
  drawReading();
  drawPlot(nowMs);
}

void BatteryInfoScreen::draw() {
  if (!_display.isReady()) return;

  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Battery"));
  drawReading();
  drawPlot(_lastRefreshMs);
}

void BatteryInfoScreen::drawReading() {
  if (!_display.isReady()) return;

  String voltageText(_reading.voltage, 3);
  voltageText += F(" V");
  String percentageText(_reading.percentage);
  percentageText += '%';

  TFT_eSPI &tft = _display.tft();
  UiKit::drawListCell(tft, _display.width(), ReadingTop, F("Raw voltage"), voltageText);
  UiKit::drawListCell(tft, _display.width(), ReadingTop + UiKit::ListCellHeight,
                      F("Charge"), percentageText);
}

void BatteryInfoScreen::drawPlot(uint32_t nowMs) {
  if (!_display.isReady()) return;

  discardExpiredSamples(nowMs);
  float minimumVoltage = _reading.voltage;
  float maximumVoltage = _reading.voltage;
  float minimumPercentage = _reading.percentage;
  float maximumPercentage = _reading.percentage;
  for (uint16_t index = 0; index < _sampleCount; index++) {
    const BatteryReading &reading = sampleAt(index).reading;
    if (reading.voltage < minimumVoltage) minimumVoltage = reading.voltage;
    if (reading.voltage > maximumVoltage) maximumVoltage = reading.voltage;
    if (reading.percentage < minimumPercentage) minimumPercentage = reading.percentage;
    if (reading.percentage > maximumPercentage) maximumPercentage = reading.percentage;
  }
  PlotRange voltageRange = paddedRange(minimumVoltage, maximumVoltage, MinimumVoltageRange);
  PlotRange percentageRange = paddedRange(minimumPercentage, maximumPercentage,
                                          MinimumPercentageRange);
  if (voltageRange.minimum < 0.0f) voltageRange.minimum = 0.0f;
  if (percentageRange.minimum < 0.0f) percentageRange.minimum = 0.0f;

  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, PlotTop - 28, _display.width(),
               _display.height() - (PlotTop - 28), TFT_BLACK);

  const int16_t right = plotRight();

  const int16_t plotHeight = PlotBottom - PlotTop;
  for (uint8_t step = 0; step <= HorizontalGridSteps; step++) {
    const float position = static_cast<float>(step) / HorizontalGridSteps;
    const int16_t y = PlotBottom - static_cast<int16_t>(position * plotHeight + 0.5f);
    tft.drawWideLine(PlotLeft, y, right, y, 1.0f, AxisColor, TFT_BLACK);

    const float voltageValue = voltageRange.minimum +
                               (voltageRange.maximum - voltageRange.minimum) * position;
    const float percentageValue = percentageRange.minimum +
                                  (percentageRange.maximum - percentageRange.minimum) * position;
    String voltageLabel(voltageValue, 2);
    voltageLabel += F("V");
    String percentageLabel(percentageValue, 1);
    percentageLabel += '%';
    UiText::draw(tft, voltageLabel, 0, y,
                 VoltageColor, TFT_BLACK, UiText::Size::Compact, ML_DATUM);
    UiText::draw(tft, percentageLabel, _display.width(), y,
                 PercentageColor, TFT_BLACK, UiText::Size::Compact, MR_DATUM);
  }

  for (uint16_t seconds = VerticalGridSeconds; seconds < plotWidth();
       seconds += VerticalGridSeconds) {
    const int16_t x = right - seconds;
    tft.drawWideLine(x, PlotTop, x, PlotBottom, 1.0f, AxisColor, TFT_BLACK);
    if (seconds % TimeLabelSeconds == 0) {
      String timeLabel = F("-");
      timeLabel += seconds;
      timeLabel += F("s");
      UiText::draw(tft, timeLabel, x, PlotBottom + 12,
                   TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
    }
  }

  tft.drawWideLine(PlotLeft, PlotTop, PlotLeft, PlotBottom, 1.0f,
                   VoltageColor, TFT_BLACK);
  tft.drawWideLine(right, PlotTop, right, PlotBottom, 1.0f,
                   PercentageColor, TFT_BLACK);
  tft.drawWideLine(PlotLeft, PlotBottom, right, PlotBottom, 1.0f,
                   AxisColor, TFT_BLACK);

  String oldestTime = F("-");
  oldestTime += plotWidth();
  oldestTime += F("s");
  UiText::draw(tft, oldestTime, PlotLeft, PlotBottom + 12,
               TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact, TL_DATUM);
  UiText::draw(tft, F("now"), right, PlotBottom + 12,
               TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact, TR_DATUM);

  drawTrace(nowMs, voltageRange.minimum, voltageRange.maximum, true);
  drawTrace(nowMs, percentageRange.minimum, percentageRange.maximum, false);
}

void BatteryInfoScreen::drawTrace(uint32_t nowMs,
                                  float scaleMinimum,
                                  float scaleMaximum,
                                  bool voltage) {
  const uint16_t color = voltage ? VoltageColor : PercentageColor;
  bool hasPrevious = false;
  int16_t previousX = 0;
  int16_t previousY = 0;
  int16_t latestX = 0;
  int16_t latestY = 0;

  for (uint16_t index = 0; index < _sampleCount; index++) {
    const Sample &sample = sampleAt(index);
    const int16_t x = sampleX(sample, nowMs);
    if (x < PlotLeft || x > plotRight()) continue;
    const float value = voltage ? sample.reading.voltage : sample.reading.percentage;
    const int16_t y = sampleY(value, scaleMinimum, scaleMaximum);
    if (hasPrevious) {
      _display.tft().drawWideLine(previousX, previousY, x, y, 1.5f,
                                  color, TFT_BLACK);
    }
    hasPrevious = true;
    previousX = x;
    previousY = y;
    latestX = x;
    latestY = y;
  }

  if (hasPrevious) {
    _display.tft().fillSmoothCircle(latestX, latestY, 2, color, TFT_BLACK);
  }
}

void BatteryInfoScreen::appendSample(uint32_t nowMs) {
  if (_sampleCount == MaxHistorySamples) {
    _sampleStart = (_sampleStart + 1) % MaxHistorySamples;
    _sampleCount--;
  }
  const uint16_t index = (_sampleStart + _sampleCount) % MaxHistorySamples;
  _samples[index] = {_reading, nowMs};
  _sampleCount++;
  discardExpiredSamples(nowMs);
}

void BatteryInfoScreen::discardExpiredSamples(uint32_t nowMs) {
  const uint32_t historyWindowMs = static_cast<uint32_t>(plotWidth()) * RefreshIntervalMs;
  while (_sampleCount > 0 && nowMs - sampleAt(0).sampledMs > historyWindowMs) {
    _sampleStart = (_sampleStart + 1) % MaxHistorySamples;
    _sampleCount--;
  }
}

const BatteryInfoScreen::Sample &BatteryInfoScreen::sampleAt(uint16_t index) const {
  return _samples[(_sampleStart + index) % MaxHistorySamples];
}

int16_t BatteryInfoScreen::plotRight() const {
  return _display.width() - PlotRightGutter;
}

int16_t BatteryInfoScreen::plotWidth() const {
  return plotRight() - PlotLeft;
}

int16_t BatteryInfoScreen::sampleX(const Sample &sample, uint32_t nowMs) const {
  const uint32_t elapsedSeconds = (nowMs - sample.sampledMs) / RefreshIntervalMs;
  return plotRight() - static_cast<int16_t>(elapsedSeconds);
}

int16_t BatteryInfoScreen::sampleY(float value,
                                   float scaleMinimum,
                                   float scaleMaximum) const {
  const float scaleRange = scaleMaximum - scaleMinimum;
  if (scaleRange <= 0.0f) return PlotBottom;
  const float ratio = (value - scaleMinimum) / scaleRange;
  const int16_t plotHeight = PlotBottom - PlotTop;
  int16_t y = PlotBottom - static_cast<int16_t>(ratio * plotHeight + 0.5f);
  if (y < PlotTop) y = PlotTop;
  if (y > PlotBottom) y = PlotBottom;
  return y;
}
