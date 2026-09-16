#include "AssistantScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"

#include <string.h>

namespace {
uint16_t nextUtf8Offset(const String &text, uint16_t offset) {
  if (offset >= text.length()) return offset;
  const uint8_t first = static_cast<uint8_t>(text[offset]);
  uint8_t bytes = first < 0x80 ? 1 :
                  (first >= 0xc2 && first <= 0xdf ? 2 :
                  (first >= 0xe0 && first <= 0xef ? 3 :
                  (first >= 0xf0 && first <= 0xf4 ? 4 : 1)));
  if (offset + bytes > text.length()) bytes = 1;
  return offset + bytes;
}

uint8_t wrapCaption(const String &text, String *lines, uint8_t capacity,
                    int16_t maxWidth) {
  if (!lines || capacity == 0 || text.length() == 0) return 0;
  uint16_t position = 0;
  uint8_t count = 0;
  while (position < text.length() && count < capacity) {
    while (position < text.length() && text[position] == ' ') position++;
    const uint16_t start = position;
    uint16_t end = position;
    int32_t lastSpace = -1;
    bool completed = false;
    while (end < text.length()) {
      if (text[end] == '\r') {
        end++;
        continue;
      }
      if (text[end] == '\n') {
        lines[count] = text.substring(start, end);
        lines[count].trim();
        position = end + 1;
        completed = true;
        break;
      }
      if (text[end] == ' ') lastSpace = end;
      const uint16_t next = nextUtf8Offset(text, end);
      if (UiText::measure(text.substring(start, next), UiText::Size::Compact) > maxWidth) {
        uint16_t split = lastSpace >= static_cast<int32_t>(start)
          ? static_cast<uint16_t>(lastSpace) : end;
        if (split == start) split = next;
        lines[count] = text.substring(start, split);
        lines[count].trim();
        position = lastSpace >= static_cast<int32_t>(start)
          ? static_cast<uint16_t>(lastSpace + 1) : split;
        completed = true;
        break;
      }
      end = next;
    }
    if (!completed) {
      lines[count] = text.substring(start);
      lines[count].trim();
      position = text.length();
    }
    count++;
  }
  if (position < text.length() && count > 0) {
    String remainder = lines[count - 1] + " " + text.substring(position);
    remainder.trim();
    lines[count - 1] = UiText::ellipsize(remainder, maxWidth, UiText::Size::Compact);
  }
  return count;
}

bool sameText(const char *left, const char *right) {
  return strcmp(left ? left : "", right ? right : "") == 0;
}
}  // namespace

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

bool AssistantScreen::releaseResources() {
  if (!_controller.snapshot().sessionActive) return true;
  _closePending = true;
  _openingWiFi = false;
  _lastCloseRequestMs = millis();
  _controller.deactivate();
  return false;
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
  const bool avatarChanged = next.phase != _snapshot.phase ||
                             next.emotion != _snapshot.emotion;
  const bool textChanged = next.phase != _snapshot.phase ||
                           !sameText(next.status, _snapshot.status) ||
                           !sameText(next.activationMessage, _snapshot.activationMessage) ||
                           !sameText(next.activationCode, _snapshot.activationCode) ||
                           !sameText(next.sttCaption, _snapshot.sttCaption) ||
                           !sameText(next.ttsCaption, _snapshot.ttsCaption);
  const bool buttonChanged = next.phase != _snapshot.phase ||
                             next.retryable != _snapshot.retryable;
  _snapshot = next;
  if (avatarChanged ||
      (avatarAnimated(_snapshot.phase) && nowMs - _lastAnimationMs >= 100)) {
    drawAvatar(nowMs);
    _lastAnimationMs = nowMs;
  }
  if (textChanged) drawText();
  if (buttonChanged) drawButton();
}

void AssistantScreen::requestClose(bool openWiFi) {
  _closePending = true;
  _openingWiFi = openWiFi;
  _lastCloseRequestMs = millis();
  drawButton();
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
  const uint32_t nowMs = millis();
  drawAvatar(nowMs);
  drawText();
  drawButton();
  _lastAnimationMs = nowMs;
}

void AssistantScreen::drawAvatar(uint32_t nowMs) {
  if (!_display.isReady()) return;
  _avatar.draw(_display.tft(), _display.width() / 2, AvatarCenterY, _snapshot.phase,
               _snapshot.emotion, nowMs, TFT_BLACK);
}

void AssistantScreen::drawText() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(8, TextTop, _display.width() - 16, ButtonY - TextTop - 4, TFT_BLACK);
  UiText::draw(tft, String(_snapshot.status), _display.width() / 2, TextTop + 1,
               _snapshot.phase == AssistantPhase::Error ? 0xf986 : TFT_WHITE,
               TFT_BLACK, UiText::Size::Content, TC_DATUM);

  String caption;
  uint16_t captionColor = TFT_CYAN;
  if (_snapshot.phase == AssistantPhase::NeedsActivation) {
    caption = String(_snapshot.activationMessage);
    if (_snapshot.activationCode[0]) caption += "\n" + String(_snapshot.activationCode);
  } else if (_snapshot.ttsCaption[0]) {
    // Once an answer is available it owns the complete caption area. The STT
    // remains visible while waiting/thinking, but never crops the reply.
    caption = String(_snapshot.ttsCaption);
  } else {
    caption = String(_snapshot.sttCaption);
    captionColor = TFT_LIGHTGREY;
  }
  caption = UiText::normalizeForFont(caption, UiText::Size::Compact);
  String lines[MaxCaptionLines];
  const uint8_t lineCount = wrapCaption(caption, lines, MaxCaptionLines,
                                        _display.width() - 32);
  for (uint8_t index = 0; index < lineCount; ++index) {
    UiText::draw(tft, lines[index], _display.width() / 2,
                 CaptionTop + index * UiText::lineHeight(UiText::Size::Compact),
                 captionColor, TFT_BLACK, UiText::Size::Compact, TC_DATUM);
  }
}

void AssistantScreen::drawButton() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  const bool enabled = _snapshot.phase == AssistantPhase::Ready || active() ||
                       _snapshot.phase == AssistantPhase::NeedsWiFi ||
                       (_snapshot.phase == AssistantPhase::Error && _snapshot.retryable);
  UiKit::drawFilledButton(tft, ButtonX, ButtonY, ButtonWidth, ButtonHeight,
                          _closePending ? String(F("Closing")) : String(buttonLabel()),
                          enabled && !_closePending ? 0x151f : TFT_DARKGREY,
                          TFT_WHITE, TFT_BLACK);
}

bool AssistantScreen::avatarAnimated(AssistantPhase phase) {
  return phase == AssistantPhase::Listening ||
         phase == AssistantPhase::Thinking ||
         phase == AssistantPhase::Speaking;
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
