#include "AppHostScreen.h"

#include <time.h>

#include "../../app/Navigator.h"
#include "../../services/network/WiFiController.h"
#include "../../services/storage/BatteryHistoryService.h"
#include "../../services/storage/SdCardController.h"
#include "../../services/audio/SpeakerController.h"
#include "../../services/audio/AudioSettings.h"
#include "../../media/FileFormats.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"

AppHostScreen::AppHostScreen(Navigator &navigator, TouchController &touch,
                             DisplayController &display, SdCardController &card,
                             WiFiController &wifi, BatteryHistoryService &batteryHistory,
                             SpeakerController &speaker, AudioSettings &audioSettings,
                             MicController &mic, MediaCache &mediaCache)
  : Screen(navigator, touch, display), _card(card), _wifi(wifi),
    _batteryHistory(batteryHistory),
    _runtime(card, speaker, audioSettings, mic, mediaCache) {}

void AppHostScreen::onEnter(const ScreenData &data) {
  _lastTouchDown = false;
  _closeRequested = false;
  _revision = 0;
  _frameRevision = 0;
  _canvasMode = false;
  _covering = _promptPending = _promptAnswered = _resumePending = false;
  _touchActive = false;
  _promptText = String();
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

void AppHostScreen::onExit() {
  // A screen this host pushed for the app only covers it; the paused app
  // keeps its Lua state until onResume().
  if (!_covering) _runtime.requestStop();
}

bool AppHostScreen::releaseResources() {
  if (!_runtime.end()) return false;
  _running = false;
  _covering = _promptPending = _promptAnswered = _resumePending = false;
  _promptText = String();
  return true;
}

void AppHostScreen::onResume() {
  // The touch that closed the covering screen must not reach the app.
  _lastTouchDown = true;
  _touchActive = false;
  if (_covering) {
    _covering = false;
    _resumePending = true;
  } else if (_running) {
    // Another screen covered an app that was not paused; it was stopped.
    _closeRequested = true;
  }
  redraw();
}

void AppHostScreen::onResult(const ScreenResult &result) {
  if (!_promptPending || result.kind != ScreenResultKind::Text) return;
  _promptAnswered = true;
  _promptText = result.text;
}

void AppHostScreen::openNavigation() {
  const String path = _navigation.path;
  const FileFormats::Format format = FileFormats::fromPath(path);
  ScreenId target = ScreenId::TextInput;
  ScreenData data;
  bool valid = true;
  if (_navigation.kind == AppRuntime::NavigationKind::Prompt) {
    data = ScreenData::textInput(_navigation.title, false, _navigation.initial);
  } else if (!FileFormats::isOpenable(format)) {
    valid = false;
  } else if (FileFormats::isImage(format)) {
    target = ScreenId::ImageViewer; data = ScreenData::imagePath(path);
  } else if (FileFormats::isAudio(format)) {
    target = ScreenId::AudioPlayer; data = ScreenData::audioPath(path);
  } else if (FileFormats::isVideo(format)) {
    target = ScreenId::VideoPlayer; data = ScreenData::videoPath(path);
  } else {
    target = ScreenId::TextViewer; data = ScreenData::textPath(path);
  }
  _promptPending = _navigation.kind == AppRuntime::NavigationKind::Prompt;
  _promptAnswered = false;
  _promptText = String();
  _covering = true;
  if (valid && _navigator.push(target, data)) return;
  // Nothing covered the app; continue it as if the screen had been closed.
  _covering = false;
  _resumePending = true;
}

void AppHostScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    if (_touchActive) {
      _touchActive = false;
      _runtime.sendTouchEnd(_touchX, _touchY, millis() - _touchStartedMs);
    }
    return;
  }
  if (_lastTouchDown) {
    if (_touchActive && point.x >= 0 && point.x < _display.width() &&
        point.y >= UiKit::AppBarHeight && point.y < _display.height()) {
      _touchX = point.x;
      _touchY = point.y - UiKit::AppBarHeight;
    }
    return;
  }
  _lastTouchDown = true;
  if (UiKit::navigationIconContains(point.x, point.y)) {
    _closeRequested = true;
    _runtime.requestStop();
    return;
  }
  if (_canvasMode) {
    if (!_closeRequested && point.x < _display.width() &&
        point.y >= UiKit::AppBarHeight && point.y < _display.height()) {
      _touchX = point.x;
      _touchY = point.y - UiKit::AppBarHeight;
      _touchStartedMs = millis();
      _touchActive = _runtime.sendTouch(_touchX, _touchY);
    }
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
  if (_resumePending) {
    _resumePending = false;
    _runtime.resume(_promptPending && _promptAnswered ? _promptText.c_str() : nullptr);
    _promptPending = _promptAnswered = false;
    _promptText = String();
  }
  if (_runtime.takeFrame(_frame, _frameRevision)) { _canvasMode = true; drawFrame(); }
  AppRuntime::Scene scene{};
  if (_runtime.takeScene(scene, _revision)) {
    _canvasMode = false; _scene = scene; draw();
  }
  if (_closeRequested) {
    if (_runtime.stopped()) _navigator.pop();
    return;
  }
  // Navigation happens here, on the UI task, once the worker has paused.
  if (_running && _runtime.takeNavigation(_navigation)) openNavigation();
}

void AppHostScreen::publishStatus() {
  _runtime.publishStatus(statusSnapshot());
}

AppRuntime::Status AppHostScreen::statusSnapshot() {
  AppRuntime::Status status{};
  const float voltage = _batteryHistory.latest().smoothVoltage;
  const float millivolts = voltage > 0.0f ? voltage * 1000.0f + 0.5f : 0.0f;
  status.batteryVoltageMv = millivolts >= 65535.0f
      ? 65535 : static_cast<uint16_t>(millivolts);
  status.wifiConnected = _wifi.isConnected();
  status.sdMounted = _card.isMounted();
  const time_t epoch = time(nullptr);
  status.epochSeconds = epoch >= 1609459200 ? static_cast<int64_t>(epoch) : 0;
  return status;
}

void AppHostScreen::redraw() {
  if (_canvasMode) drawFrame(); else draw();
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

void AppHostScreen::drawFrame() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t width = _display.width();
  const int16_t height = _display.height() - UiKit::AppBarHeight;
  tft.setViewport(0, UiKit::AppBarHeight, width, height, true);
  tft.fillRect(0, 0, width, height, _frame.background);
  for (uint8_t i = 0; i < _frame.count; ++i) {
    const AppRuntime::DrawCommand &command = _frame.commands[i];
    const int32_t *a = command.arg;
    switch (command.op) {
      case AppRuntime::DrawOp::FillScreen: tft.fillRect(0, 0, width, height, a[0]); break;
      case AppRuntime::DrawOp::FillRect: tft.fillRect(a[0], a[1], a[2], a[3], a[4]); break;
      case AppRuntime::DrawOp::DrawRect: tft.drawRect(a[0], a[1], a[2], a[3], a[4]); break;
      case AppRuntime::DrawOp::DrawLine: tft.drawLine(a[0], a[1], a[2], a[3], a[4]); break;
      case AppRuntime::DrawOp::HLine: tft.drawFastHLine(a[0], a[1], a[2], a[3]); break;
      case AppRuntime::DrawOp::VLine: tft.drawFastVLine(a[0], a[1], a[2], a[3]); break;
      case AppRuntime::DrawOp::DrawCircle: tft.drawCircle(a[0], a[1], a[2], a[3]); break;
      case AppRuntime::DrawOp::FillCircle: tft.fillCircle(a[0], a[1], a[2], a[3]); break;
      case AppRuntime::DrawOp::DrawRoundRect:
        tft.drawRoundRect(a[0], a[1], a[2], a[3], a[4], a[5]); break;
      case AppRuntime::DrawOp::FillRoundRect:
        tft.fillRoundRect(a[0], a[1], a[2], a[3], a[4], a[5]); break;
      case AppRuntime::DrawOp::FillTriangle:
        tft.fillTriangle(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
      case AppRuntime::DrawOp::DrawWideLine:
        tft.drawWideLine(a[0], a[1], a[2], a[3], a[4] / 10.0f, a[5], a[6]); break;
      case AppRuntime::DrawOp::DrawSmoothCircle:
        tft.drawSmoothCircle(a[0], a[1], a[2], a[3], a[4]); break;
      case AppRuntime::DrawOp::FillSmoothCircle:
        tft.fillSmoothCircle(a[0], a[1], a[2], a[3], a[4]); break;
      case AppRuntime::DrawOp::DrawSmoothRoundRect:
        tft.drawSmoothRoundRect(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
      case AppRuntime::DrawOp::FillSmoothRoundRect:
        tft.fillSmoothRoundRect(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
      case AppRuntime::DrawOp::DrawSmoothArc:
        tft.drawSmoothArc(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8] != 0); break;
      case AppRuntime::DrawOp::Text:
        UiText::draw(tft, command.text, a[0], a[1], a[2], a[3],
                     static_cast<UiText::Size>(a[4]), static_cast<uint8_t>(a[5])); break;
      case AppRuntime::DrawOp::Button:
        UiKit::drawFilledButton(tft, a[0], a[1], a[2], a[3], command.text,
                                a[4], a[5], a[6]); break;
      case AppRuntime::DrawOp::Progress:
        UiKit::drawProgressBar(tft, a[0], a[1], a[2], a[3], a[4], false,
                               a[5], a[6], a[7], true); break;
      case AppRuntime::DrawOp::ListCell:
        UiKit::drawFileListCell(tft, width, a[0], command.text,
                                static_cast<UiKit::FileIcon>(a[1]),
                                static_cast<UiKit::ListAction>(a[2])); break;
    }
  }
  tft.resetViewport();
  UiKit::drawTopAppBar(tft, width, _frame.title);
}
