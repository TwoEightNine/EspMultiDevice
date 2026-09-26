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
  const float padding = (observedMaximum - observedMinimum) * GridPaddingFraction;
  return {observedMinimum - padding, observedMaximum + padding};
}

uint32_t timeGridSeconds(uint32_t durationSeconds) {
  static const uint32_t Intervals[] = {
    1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800,
    3600, 7200, 14400, 21600, 43200, 86400,
  };
  for (uint8_t index = 0; index < sizeof(Intervals) / sizeof(Intervals[0]); ++index) {
    if (durationSeconds <= Intervals[index] * 5UL) return Intervals[index];
  }
  return Intervals[sizeof(Intervals) / sizeof(Intervals[0]) - 1];
}

String elapsedLabel(uint32_t elapsedSeconds) {
  String label;
  if (elapsedSeconds < 60) {
    label = elapsedSeconds;
    label += F("s");
  } else if (elapsedSeconds < 3600) {
    label = elapsedSeconds / 60;
    label += F("m");
  } else {
    label = elapsedSeconds / 3600;
    label += F("h");
  }
  return label;
}

float pointVoltage(const BatteryHistoryPoint &point) {
  return point.sampleCount == 0 ? 0.0f : point.smoothVoltageTotal / point.sampleCount;
}

float pointPercentage(const BatteryHistoryPoint &point) {
  return point.sampleCount == 0 ? 0.0f : point.smoothPercentageTotal / point.sampleCount;
}
}  // namespace

BatteryInfoScreen::BatteryInfoScreen(Navigator &navigator,
                                     TouchController &touch,
                                     DisplayController &display,
                                     BatteryHistoryService &history)
  : Screen(navigator, touch, display),
    _history(history),
    _lastTouchDown(false),
    _lastDrawnGeneration(0) {
}

void BatteryInfoScreen::onEnter() {
  _lastTouchDown = false;
  draw();
  _lastDrawnGeneration = _history.generation();
}

void BatteryInfoScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;
  if (UiKit::navigationIconContains(point.x, point.y)) _navigator.pop();
}

void BatteryInfoScreen::tick(uint32_t nowMs) {
  (void)nowMs;
  const uint32_t generation = _history.generation();
  if (generation == _lastDrawnGeneration) return;
  drawReading();
  drawPlot();
  _lastDrawnGeneration = generation;
}

void BatteryInfoScreen::draw() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Battery"));
  drawReading();
  drawPlot();
}

void BatteryInfoScreen::drawReading() {
  if (!_display.isReady()) return;
  const BatteryTelemetryReading &reading = _history.latest();
  String rawVoltage(reading.raw.voltage, 3);
  rawVoltage += F(" V");
  String smoothVoltage(reading.smooth.voltage, 3);
  smoothVoltage += F(" V");
  String rawPercentage(reading.raw.percentage);
  rawPercentage += '%';
  String smoothPercentage(reading.smooth.percentage);
  smoothPercentage += '%';

  TFT_eSPI &tft = _display.tft();
  const int16_t rowHeight = UiKit::ListCellHeight;
  UiKit::drawListCell(tft, _display.width(), ReadingTop, F("Raw voltage"), rawVoltage);
  UiKit::drawListCell(tft, _display.width(), ReadingTop + rowHeight,
                      F("Smooth voltage"), smoothVoltage);
  UiKit::drawListCell(tft, _display.width(), ReadingTop + rowHeight * 2,
                      F("Raw charge"), rawPercentage);
  UiKit::drawListCell(tft, _display.width(), ReadingTop + rowHeight * 3,
                      F("Smooth charge"), smoothPercentage);
}

void BatteryInfoScreen::drawPlot() {
  if (!_display.isReady()) return;
  const uint16_t count = _history.historyCount();
  const BatteryTelemetryReading &latest = _history.latest();
  float minimumVoltage = latest.smooth.voltage;
  float maximumVoltage = latest.smooth.voltage;
  float minimumPercentage = latest.smooth.percentage;
  float maximumPercentage = latest.smooth.percentage;
  for (uint16_t index = 0; index < count; ++index) {
    const BatteryHistoryPoint &point = _history.historyPoint(index);
    const float voltage = pointVoltage(point);
    const float percentage = pointPercentage(point);
    if (voltage < minimumVoltage) minimumVoltage = voltage;
    if (voltage > maximumVoltage) maximumVoltage = voltage;
    if (percentage < minimumPercentage) minimumPercentage = percentage;
    if (percentage > maximumPercentage) maximumPercentage = percentage;
  }

  PlotRange voltageRange = paddedRange(minimumVoltage, maximumVoltage, MinimumVoltageRange);
  PlotRange percentageRange = paddedRange(minimumPercentage, maximumPercentage,
                                          MinimumPercentageRange);
  if (voltageRange.minimum < 0.0f) voltageRange.minimum = 0.0f;
  if (percentageRange.minimum < 0.0f) percentageRange.minimum = 0.0f;

  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, PlotTop - 20, _display.width(),
               _display.height() - (PlotTop - 20), TFT_BLACK);
  const int16_t right = plotRight();
  const int16_t plotHeight = PlotBottom - PlotTop;
  for (uint8_t step = 0; step <= HorizontalGridSteps; ++step) {
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
    UiText::draw(tft, voltageLabel, 0, y, VoltageColor, TFT_BLACK,
                 UiText::Size::Compact, ML_DATUM);
    UiText::draw(tft, percentageLabel, _display.width(), y, PercentageColor, TFT_BLACK,
                 UiText::Size::Compact, MR_DATUM);
  }

  const uint32_t durationMs = _history.historyDurationMs();
  const uint32_t durationSeconds = max(1UL, (durationMs + 999UL) / 1000UL);
  const uint32_t gridSeconds = timeGridSeconds(durationSeconds);
  for (uint32_t seconds = gridSeconds; seconds < durationSeconds; seconds += gridSeconds) {
    const int16_t x = PlotLeft + static_cast<int16_t>(
        (static_cast<uint64_t>(seconds) * (right - PlotLeft)) / durationSeconds);
    tft.drawWideLine(x, PlotTop, x, PlotBottom, 1.0f, AxisColor, TFT_BLACK);
    UiText::draw(tft, elapsedLabel(seconds), x, PlotBottom + 12, TFT_LIGHTGREY, TFT_BLACK,
                 UiText::Size::Compact, MC_DATUM);
  }

  tft.drawWideLine(PlotLeft, PlotTop, PlotLeft, PlotBottom, 1.0f, VoltageColor, TFT_BLACK);
  tft.drawWideLine(right, PlotTop, right, PlotBottom, 1.0f, PercentageColor, TFT_BLACK);
  tft.drawWideLine(PlotLeft, PlotBottom, right, PlotBottom, 1.0f, AxisColor, TFT_BLACK);
  UiText::draw(tft, F("boot"), PlotLeft, PlotBottom + 12, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, TL_DATUM);
  UiText::draw(tft, F("now"), right, PlotBottom + 12, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, TR_DATUM);

  drawTrace(voltageRange.minimum, voltageRange.maximum, true);
  drawTrace(percentageRange.minimum, percentageRange.maximum, false);
}

void BatteryInfoScreen::drawTrace(float scaleMinimum, float scaleMaximum, bool voltage) {
  const uint16_t color = voltage ? VoltageColor : PercentageColor;
  const uint32_t durationMs = _history.historyDurationMs();
  bool hasPrevious = false;
  int16_t previousX = 0;
  int16_t previousY = 0;
  int16_t latestX = 0;
  int16_t latestY = 0;
  for (uint16_t index = 0; index < _history.historyCount(); ++index) {
    const BatteryHistoryPoint &point = _history.historyPoint(index);
    const float value = voltage ? pointVoltage(point) : pointPercentage(point);
    const int16_t x = sampleX(point, durationMs);
    const int16_t y = sampleY(value, scaleMinimum, scaleMaximum);
    if (hasPrevious) {
      _display.tft().drawWideLine(previousX, previousY, x, y, 1.5f, color, TFT_BLACK);
    }
    hasPrevious = true;
    previousX = x;
    previousY = y;
    latestX = x;
    latestY = y;
  }
  if (hasPrevious) _display.tft().fillSmoothCircle(latestX, latestY, 2, color, TFT_BLACK);
}

int16_t BatteryInfoScreen::plotRight() const {
  return _display.width() - PlotRightGutter;
}

int16_t BatteryInfoScreen::sampleX(const BatteryHistoryPoint &point, uint32_t durationMs) const {
  if (durationMs == 0) return plotRight();
  const uint32_t midpointMs = point.firstElapsedMs +
                              (point.lastElapsedMs - point.firstElapsedMs) / 2;
  return PlotLeft + static_cast<int16_t>(
      (static_cast<uint64_t>(midpointMs) * (plotRight() - PlotLeft)) / durationMs);
}

int16_t BatteryInfoScreen::sampleY(float value, float scaleMinimum, float scaleMaximum) const {
  const float scaleRange = scaleMaximum - scaleMinimum;
  if (scaleRange <= 0.0f) return PlotBottom;
  const float ratio = (value - scaleMinimum) / scaleRange;
  const int16_t plotHeight = PlotBottom - PlotTop;
  int16_t y = PlotBottom - static_cast<int16_t>(ratio * plotHeight + 0.5f);
  if (y < PlotTop) y = PlotTop;
  if (y > PlotBottom) y = PlotBottom;
  return y;
}
