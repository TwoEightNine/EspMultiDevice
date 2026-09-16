#include "AssistantScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"

AssistantScreen::AssistantScreen(Navigator &navigator, TouchController &touch,
                                 DisplayController &display,
                                 AssistantController &controller)
    : Screen(navigator, touch, display), _controller(controller), _avatar(),
      _snapshot{}, _lastTouchDown(false), _closePending(false),
      _openingWiFi(false), _lastAnimationMs(0), _lastCloseRequestMs(0) {
  _snapshot.clear();
}

void AssistantScreen::onEnter() { enter(); }
void AssistantScreen::onResume() { enter(); }

void AssistantScreen::enter() {
  _lastTouchDown = false;
  _closePending = false;
  _openingWiFi = false;
  _lastAnimationMs = 0;
  _lastCloseRequestMs = 0;
  _controller.activate();
  _snapshot = _controller.snapshot();
  draw(true);
}

void AssistantScreen::onExit() {
  if (_controller.snapshot().sessionActive) _controller.deactivate();
}

void AssistantScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown || _closePending) return;
  _lastTouchDown = true;
  if (UiKit::navigationIconContains(point.x, point.y)) {
    requestClose(false);
    return;
  }
  if (point.x < ButtonX || point.x >= ButtonX + ButtonWidth ||
      point.y < ButtonY || point.y >= ButtonY + ButtonHeight) return;
  if (_snapshot.phase == AssistantPhase::Ready) {
    _controller.startTurn();
  } else if (active()) {
    _controller.cancelTurn();
  } else if (_snapshot.phase == AssistantPhase::NeedsWiFi) {
    requestClose(true);
  } else if (_snapshot.phase == AssistantPhase::Error && _snapshot.retryable) {
    _controller.retry();
  }
}

void AssistantScreen::tick(uint32_t nowMs) {
  _controller.tick(nowMs);
  const AssistantSnapshot next = _controller.snapshot();
  if (_closePending && next.sessionActive && nowMs - _lastCloseRequestMs >= 250) {
    _controller.deactivate();
    _lastCloseRequestMs = nowMs;
  }
  if (_closePending && !next.sessionActive) {
    if (_openingWiFi) _navigator.push(ScreenId::WiFi);
    else _navigator.pop();
    return;
  }
  const bool changed = next.revision != _snapshot.revision;
  _snapshot = next;
  if (changed || nowMs - _lastAnimationMs >= 100) {
    drawContent(nowMs);
    _lastAnimationMs = nowMs;
  }
}

void AssistantScreen::requestClose(bool openWiFi) {
  _closePending = true;
  _openingWiFi = openWiFi;
  _lastCloseRequestMs = millis();
  if (_snapshot.sessionActive) _controller.deactivate();
  else if (openWiFi) _navigator.push(ScreenId::WiFi);
  else _navigator.pop();
}

void AssistantScreen::draw(bool full) {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  if (full) {
    tft.fillScreen(TFT_BLACK);
    UiKit::drawTopAppBar(tft, _display.width(), F("Assistant"));
  }
  drawContent(millis());
}

void AssistantScreen::drawContent(uint32_t nowMs) {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  _avatar.draw(tft, _display.width() / 2, 112, _snapshot.phase,
               _snapshot.emotion, nowMs, TFT_BLACK);
  tft.fillRect(8, 177, _display.width() - 16, 87, TFT_BLACK);
  UiText::draw(tft, String(_snapshot.status), _display.width() / 2, 183,
               _snapshot.phase == AssistantPhase::Error ? 0xf986 : TFT_WHITE,
               TFT_BLACK, UiText::Size::Content, TC_DATUM);

  String first;
  String second;
  if (_snapshot.phase == AssistantPhase::NeedsActivation) {
    first = String(_snapshot.activationMessage);
    second = String(_snapshot.activationCode);
  } else {
    first = String(_snapshot.sttCaption);
    second = String(_snapshot.ttsCaption);
  }
  first = UiText::normalizeForFont(first, UiText::Size::Compact);
  second = UiText::normalizeForFont(second, UiText::Size::Compact);
  first = UiText::ellipsize(first, _display.width() - 40, UiText::Size::Compact);
  second = UiText::ellipsize(second, _display.width() - 40, UiText::Size::Compact);
  UiText::draw(tft, first, _display.width() / 2, 218, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, TC_DATUM);
  UiText::draw(tft, second, _display.width() / 2, 241, TFT_CYAN, TFT_BLACK,
               UiText::Size::Compact, TC_DATUM);

  const bool enabled = _snapshot.phase == AssistantPhase::Ready || active() ||
                       _snapshot.phase == AssistantPhase::NeedsWiFi ||
                       (_snapshot.phase == AssistantPhase::Error && _snapshot.retryable);
  UiKit::drawFilledButton(tft, ButtonX, ButtonY, ButtonWidth, ButtonHeight,
                          _closePending ? String(F("Closing")) : String(buttonLabel()),
                          enabled && !_closePending ? 0x151f : TFT_DARKGREY,
                          TFT_WHITE, TFT_BLACK);
}

const char *AssistantScreen::buttonLabel() const {
  if (_snapshot.phase == AssistantPhase::Ready) return "Talk";
  if (active()) return "Cancel";
  if (_snapshot.phase == AssistantPhase::NeedsWiFi) return "Wi-Fi settings";
  if (_snapshot.phase == AssistantPhase::Error && _snapshot.retryable) return "Retry";
  return "Unavailable";
}

bool AssistantScreen::active() const {
  return _snapshot.phase == AssistantPhase::Bootstrapping ||
         _snapshot.phase == AssistantPhase::NeedsActivation ||
         _snapshot.phase == AssistantPhase::Connecting ||
         _snapshot.phase == AssistantPhase::Listening ||
         _snapshot.phase == AssistantPhase::Thinking ||
         _snapshot.phase == AssistantPhase::Speaking;
}
