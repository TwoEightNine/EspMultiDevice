#include "HomeScreen.h"

#include "Navigator.h"

namespace {
static const int16_t TOUCH_LOG_LINE_HEIGHT = 18;
}

HomeScreen::HomeScreen(Navigator &navigator,
                       TouchController &touch,
                       DisplayController &display,
                       SdCardController &sdCard)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _sdMounted(false),
    _sdDetail(F("Touch ready, mounting SD card...")),
    _touchLogCount(0),
    _touchLogNext(0),
    _touchEventCount(0),
    _lastTouchDown(false) {
}

void HomeScreen::onEnter() {
  refreshSdState();
}

void HomeScreen::handleTouch(const TouchPoint &point) {
  _touchEventCount++;

  if (!point.touched) {
    _lastTouchDown = false;
    Serial.println(F("Touch released."));
    String message = String(_touchEventCount);
    message += F(": released");
    logTouch(message);
    return;
  }

  if (!_lastTouchDown && _recorderButton.contains(point.x, point.y)) {
    _lastTouchDown = true;
    _navigator.push(ScreenId::VoiceRecorder);
    return;
  }

  _lastTouchDown = true;

  Serial.print(F("Touch x="));
  Serial.print(point.x);
  Serial.print(F(" y="));
  Serial.print(point.y);
  Serial.print(F(" z="));
  Serial.print(point.z);
  Serial.print(F(" rawX="));
  Serial.print(point.rawX);
  Serial.print(F(" rawY="));
  Serial.println(point.rawY);

  if (_display.isReady()) {
    _display.tft().fillCircle(point.x, point.y, 4, TFT_YELLOW);
  }

  String message = String(_touchEventCount);
  message += F(": x=");
  message += point.x;
  message += F(" y=");
  message += point.y;
  message += F(" z=");
  message += point.z;
  message += F(" raw=");
  message += point.rawX;
  message += F(",");
  message += point.rawY;
  logTouch(message);
}

void HomeScreen::tick(uint32_t nowMs) {
  (void)nowMs;

  handleSdCardEvent(_sdCard.update());
}

void HomeScreen::draw() {
  if (!_display.isReady()) {
    return;
  }

  layoutButtons();
  _display.tft().fillRect(0, 0, _display.width(), 110, TFT_BLACK);
  _display.tft().setTextDatum(TL_DATUM);
  _display.tft().setTextColor(TFT_WHITE, TFT_BLACK);
  String status = "SD mounted: " + String(_sdMounted) + ". " + _sdDetail;
  _display.tft().drawString(status, 10, 10, 2);
  drawRecorderButton();
  redrawTouchLog();
}

void HomeScreen::layoutButtons() {
  _recorderButton = { 10, 46, 132, 42 };
}

void HomeScreen::drawRecorderButton() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRoundRect(_recorderButton.x, _recorderButton.y, _recorderButton.w, _recorderButton.h, 6, TFT_CYAN);
  tft.drawRoundRect(_recorderButton.x, _recorderButton.y, _recorderButton.w, _recorderButton.h, 6, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_BLACK, TFT_CYAN);
  tft.drawString(F("Recorder"), _recorderButton.x + _recorderButton.w / 2, _recorderButton.y + _recorderButton.h / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void HomeScreen::redrawTouchLog() {
  if (!_display.isReady()) {
    return;
  }

  TFT_eSPI &tft = _display.tft();
  int16_t width = _display.width();
  int16_t height = _display.height();
  int16_t logHeight = TOUCH_LOG_LINE_HEIGHT * (TouchLogLines + 1) + 8;
  int16_t top = height - logHeight;

  tft.fillRect(0, top, width, logHeight, TFT_BLACK);
  tft.drawFastHLine(8, top, width - 16, TFT_DARKGREY);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(F("Touch log"), 10, top + 5, 2);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  for (uint8_t i = 0; i < _touchLogCount; i++) {
    uint8_t index = (_touchLogNext + TouchLogLines - _touchLogCount + i) % TouchLogLines;
    tft.drawString(_touchLog[index], 10, top + 25 + i * TOUCH_LOG_LINE_HEIGHT, 2);
  }
}

void HomeScreen::logTouch(const String &message) {
  _touchLog[_touchLogNext] = message;
  _touchLogNext = (_touchLogNext + 1) % TouchLogLines;

  if (_touchLogCount < TouchLogLines) {
    _touchLogCount++;
  }

  redrawTouchLog();
}

void HomeScreen::setSdState(bool mounted, const String &detail) {
  _sdMounted = mounted;
  _sdDetail = detail;
  draw();
}

void HomeScreen::refreshSdState() {
  if (_sdCard.isMounted()) {
    setSdState(true, F("Mounted"));
    runSdSelfTest();
    return;
  }

  setSdState(false, F("Waiting for card"));
}

void HomeScreen::runSdSelfTest() {
  if (!_sdCard.isMounted()) {
    return;
  }

  if (_sdCard.runReadWriteTest()) {
    Serial.println(F("SD read/write test passed."));
    setSdState(true, F("Read/write test passed"));
  } else {
    Serial.println(F("SD read/write test failed."));
    setSdState(false, F("Read/write test failed"));
  }
}

void HomeScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::Mounted) {
    Serial.println(F("SD card inserted and mounted."));
    setSdState(true, F("Inserted and mounted"));
    runSdSelfTest();
  } else if (event == SdCardEvent::Removed) {
    Serial.println(F("SD card removed or became unavailable."));
    setSdState(false, F("Removed or unavailable"));
  } else if (event == SdCardEvent::MountFailed) {
    setSdState(false, F("Mount retry failed"));
  }
}
