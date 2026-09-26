#include "AppHostScreen.h"

#include <time.h>

#include "../../app/Navigator.h"
#include "../../core/BatteryController.h"
#include "../../services/network/WiFiController.h"
#include "../../services/storage/SdCardController.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"

AppHostScreen::AppHostScreen(Navigator &navigator, TouchController &touch,
                             DisplayController &display, SdCardController &card,
                             WiFiController &wifi, BatteryController &battery)
  : Screen(navigator, touch, display), _card(card), _wifi(wifi),
    _battery(battery), _runtime(card) {}

void AppHostScreen::onEnter(const ScreenData &data) {
  _lastTouchDown = false;
  _closeRequested = false;
  _revision = 0;
  _lastStatusMs = 0;
  _scene = AppRuntime::Scene{};
  snprintf(_scene.title, sizeof(_scene.title), "Apps");
  _scene.count = 1;
  snprintf(_scene.rows[0].text, sizeof(_scene.rows[0].text), "Loading app...");
  draw();
  _running = data.kind == ScreenDataKind::ExternalApp &&
             _runtime.start(data.text, statusSnapshot());
  if (!_running) {
    snprintf(_scene.title, sizeof(_scene.title), "App error");
    snprintf(_scene.rows[0].text, sizeof(_scene.rows[0].text), "Cannot start app");
    draw();
  }
}

void AppHostScreen::onExit() { _runtime.requestStop(); }

bool AppHostScreen::releaseResources() {
  if (!_runtime.end()) return false;
  _running = false;
  return true;
}

void AppHostScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) { _lastTouchDown = false; return; }
  if (_lastTouchDown) return;
  _lastTouchDown = true;
  if (UiKit::navigationIconContains(point.x, point.y)) {
    _closeRequested = true;
    _runtime.requestStop();
    return;
  }
  if (_closeRequested || point.x < 0 || point.x >= _display.width() ||
      point.y < RowTop || point.y >= RowTop + _scene.count * UiKit::ListCellHeight) return;
  const uint8_t index = (point.y - RowTop) / UiKit::ListCellHeight;
  if (_scene.rows[index].button) _runtime.sendAction(_scene.rows[index].id);
}

void AppHostScreen::tick(uint32_t nowMs) {
  if (_running && !_card.isMounted()) {
    _closeRequested = true;
    _runtime.requestStop();
  }
  if (_running && (nowMs - _lastStatusMs >= 5000 || _lastStatusMs == 0)) {
    publishStatus();
    _lastStatusMs = nowMs;
  }
  AppRuntime::Scene scene{};
  if (_runtime.takeScene(scene, _revision)) { _scene = scene; draw(); }
  if (_closeRequested && _runtime.stopped()) _navigator.pop();
}

void AppHostScreen::publishStatus() {
  _runtime.publishStatus(statusSnapshot());
}

AppRuntime::Status AppHostScreen::statusSnapshot() {
  AppRuntime::Status status{};
  status.batteryPercent = _battery.readLive().percentage;
  status.wifiConnected = _wifi.isConnected();
  status.sdMounted = _card.isMounted();
  const time_t epoch = time(nullptr);
  status.epochSeconds = epoch >= 1609459200 ? static_cast<int64_t>(epoch) : 0;
  return status;
}

void AppHostScreen::draw() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), _scene.title);
  for (uint8_t index = 0; index < _scene.count; ++index) {
    const int16_t y = RowTop + index * UiKit::ListCellHeight;
    if (_scene.rows[index].button) {
      UiKit::drawFilledButton(tft, 16, y + 2, _display.width() - 32,
                              UiKit::ListCellHeight - 4, _scene.rows[index].text,
                              TFT_DARKGREY);
    } else {
      UiKit::drawListCell(tft, _display.width(), y, _scene.rows[index].text);
    }
  }
}
