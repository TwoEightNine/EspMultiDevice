#include "AssistantScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"

#include <string.h>

namespace {
constexpr uint16_t ListeningColor = 0x07E0;
constexpr uint16_t UserBubbleColor = 0x18A5;
constexpr uint16_t AssistantBubbleColor = 0x2945;
constexpr uint16_t PrimaryButtonColor = 0x249F;
constexpr uint16_t DisabledButtonColor = 0x4208;
constexpr int16_t TranscriptLineHeight = 25;
constexpr int16_t TranscriptTextInset = 8;
constexpr int16_t PopupWidth = 260;

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
    while (position < text.length() && text[position] == ' ') ++position;
    const uint16_t start = position;
    uint16_t end = position;
    int32_t lastSpace = -1;
    bool completed = false;
    while (end < text.length()) {
      if (text[end] == '\r') { ++end; continue; }
      if (text[end] == '\n') {
        lines[count] = text.substring(start, end);
        lines[count].trim();
        position = end + 1;
        completed = true;
        break;
      }
      if (text[end] == ' ') lastSpace = end;
      const uint16_t next = nextUtf8Offset(text, end);
      if (UiText::measureAssistantCaption(text.substring(start, next)) > maxWidth) {
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
    ++count;
  }
  return count;
}

bool sameText(const char *left, const char *right) {
  return strcmp(left ? left : "", right ? right : "") == 0;
}
}  // namespace

AssistantScreen::AssistantScreen(Navigator &navigator, TouchController &touch,
                                 DisplayController &display,
                                 AssistantController &controller,
                                 SdCardController &sdCard)
    : Screen(navigator, touch, display), _controller(controller), _sdCard(sdCard),
      _avatar(), _avatarLoader(sdCard), _snapshot{}, _lastTouchDown(false),
      _closePending(false), _openingWiFi(false), _lastCloseRequestMs(0),
      _lastAvatarRevision(0), _avatarAsset(AssistantAvatar::Asset::Neutral),
      _avatarAssetValid(false), _underlayDrawn(false), _menu(Menu::Closed),
      _confirmNewConversation(false), _avatarMenuBounds{0, 0, 0, 0},
      _avatarMenuRows{}, _lastAvatarCatalogRevision(0), _scrollLine(0),
      _totalLines(0), _scrollUpBounds{280, TranscriptTop, 40, 40},
      _scrollDownBounds{280, TranscriptTop + TranscriptHeight - 40, 40, 40},
      _primaryActionBounds{16, 424, 288, 40}, _rootMenuRows{} {
  _snapshot.clear();
}

void AssistantScreen::onEnter() { enter(true); }
void AssistantScreen::onResume() { enter(false); }

void AssistantScreen::enter(bool resetAvatarSelection) {
  _lastTouchDown = false;
  _closePending = false;
  _openingWiFi = false;
  _lastCloseRequestMs = 0;
  _scrollLine = 0;
  _totalLines = 0;
  _menu = Menu::Closed;
  _confirmNewConversation = false;
  _avatarLoader.begin();
  if (resetAvatarSelection) _avatarLoader.resetAvatar();
  _controller.activate();
  _snapshot = _controller.snapshot();
  _avatarAsset = AssistantAvatar::assetFor(_snapshot.phase, _snapshot.emotion);
  _avatarAssetValid = true;
  _underlayDrawn = false;
  _lastAvatarCatalogRevision = _avatarLoader.catalogRevision();
  draw(true);
}

void AssistantScreen::onExit() {
  if (_controller.snapshot().sessionActive) _controller.deactivate();
}

bool AssistantScreen::releaseResources() {
  if (_avatarLoader.running()) _avatarLoader.stop();
  const AssistantSnapshot current = _controller.snapshot();
  if (current.sessionActive) {
    _closePending = true;
    _openingWiFi = false;
    _lastCloseRequestMs = millis();
    _controller.deactivate();
  }
  if (_avatarLoader.running() || _controller.snapshot().sessionActive) return false;
  return _avatarLoader.releaseResources();
}

bool AssistantScreen::contains(const Rect &rect, int16_t x, int16_t y) const {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

void AssistantScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) { _lastTouchDown = false; return; }
  if (_lastTouchDown || _closePending) return;
  _lastTouchDown = true;

  if (_menu == Menu::Root) {
    if (contains(_rootMenuRows[0], point.x, point.y)) {
      if (_confirmNewConversation) {
        _confirmNewConversation = false;
        _menu = Menu::Closed;
        _controller.newConversation();
        draw(false);
      } else {
        _confirmNewConversation = true;
        drawRootMenu();
      }
      return;
    }
    if (contains(_rootMenuRows[1], point.x, point.y)) {
      _confirmNewConversation = false;
      _controller.setAutoListen(!_snapshot.autoListen);
      _snapshot = _controller.snapshot();
      drawRootMenu();
      drawMicrophoneStrip();
      return;
    }
    if (contains(_rootMenuRows[2], point.x, point.y)) {
      _confirmNewConversation = false;
      _menu = Menu::Avatars;
      drawAvatarMenu();
      return;
    }
    closeMenu();
    return;
  }
  if (_menu == Menu::Avatars) {
    const uint8_t count = _avatarLoader.avatarCount();
    for (uint8_t index = 0; index < count; ++index) {
      if (contains(_avatarMenuRows[index], point.x, point.y)) {
        (void)_avatarLoader.selectAvatar(index);
        closeMenu();
        return;
      }
    }
    closeMenu();
    return;
  }
  if (UiKit::navigationIconContains(point.x, point.y)) {
    requestClose(false);
    return;
  }
  if (UiKit::appBarActionContains(point.x, point.y, _display.width())) {
    _menu = Menu::Root;
    _confirmNewConversation = false;
    drawRootMenu();
    return;
  }
  if (contains(_scrollUpBounds, point.x, point.y)) {
    _scrollLine = _scrollLine > VisibleTranscriptLines
        ? _scrollLine - VisibleTranscriptLines : 0;
    drawTranscript(false);
    return;
  }
  if (contains(_scrollDownBounds, point.x, point.y)) {
    const uint16_t bottom = _totalLines > VisibleTranscriptLines
        ? _totalLines - VisibleTranscriptLines : 0;
    _scrollLine = min<uint16_t>(bottom, _scrollLine + VisibleTranscriptLines);
    drawTranscript(false);
    return;
  }
  if (!contains(_primaryActionBounds, point.x, point.y) ||
      _snapshot.resettingConversation) return;
  if (_snapshot.phase == AssistantPhase::Ready) _controller.startTurn();
  else if (active()) _controller.cancelTurn();
  else if (_snapshot.phase == AssistantPhase::NeedsWiFi) requestClose(true);
  else if (_snapshot.phase == AssistantPhase::Error && _snapshot.retryable)
    _controller.retry();
}

void AssistantScreen::tick(uint32_t nowMs) {
  handleSdCardEvent(_sdCard.takeEvent());
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
  const AssistantAvatar::Asset nextAsset =
      AssistantAvatar::assetFor(next.phase, next.emotion);
  const bool avatarChanged = !_avatarAssetValid || nextAsset != _avatarAsset ||
                             _avatarLoader.revision() != _lastAvatarRevision;
  const bool catalogChanged =
      _avatarLoader.catalogRevision() != _lastAvatarCatalogRevision;
  const bool statusChanged = next.phase != _snapshot.phase ||
      next.micAvailable != _snapshot.micAvailable ||
      next.autoListen != _snapshot.autoListen ||
      next.resettingConversation != _snapshot.resettingConversation ||
      !sameText(next.status, _snapshot.status) ||
      !sameText(next.activationMessage, _snapshot.activationMessage) ||
      !sameText(next.activationCode, _snapshot.activationCode);
  const bool transcriptChanged =
      next.transcriptRevision != _snapshot.transcriptRevision;
  const uint16_t oldBottom = _totalLines > VisibleTranscriptLines
      ? _totalLines - VisibleTranscriptLines : 0;
  const bool wasAtBottom = _scrollLine >= oldBottom;
  _snapshot = next;
  _avatarAsset = nextAsset;
  _avatarAssetValid = true;
  if (avatarChanged) { drawAvatar(); drawPrimaryAction(); }
  if (statusChanged) {
    drawMicrophoneStrip();
    drawPrimaryAction();
    if (!transcriptChanged && _snapshot.transcriptCount == 0)
      drawTranscript(wasAtBottom);
  }
  if (transcriptChanged) drawTranscript(wasAtBottom);
  if (_menu == Menu::Root && (statusChanged || transcriptChanged)) drawRootMenu();
  if (_menu == Menu::Avatars && (avatarChanged || statusChanged ||
                                  transcriptChanged || catalogChanged))
    drawAvatarMenu();
  _lastAvatarCatalogRevision = _avatarLoader.catalogRevision();
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
  if (full) {
    UiKit::drawTopAppBar(_display.tft(), _display.width(), F("Assistant"),
                         UiKit::NavigationIcon::Close, UiKit::AppBarAction::More);
    clearAvatarPane();
  }
  drawMicrophoneStrip();
  drawTranscript(full);
  drawAvatar();
  drawPrimaryAction();
  if (_menu == Menu::Root) drawRootMenu();
  else if (_menu == Menu::Avatars) drawAvatarMenu();
}

void AssistantScreen::drawAvatar() {
  if (!_display.isReady()) return;
  const int16_t x = (_display.width() - AvatarWidth) / 2;
  const int16_t y = _display.height() - AvatarHeight;
  if (!_underlayDrawn) {
    const uint16_t *pixels = nullptr;
    const uint8_t *alpha = nullptr;
    if (_avatarLoader.underlay(pixels, alpha)) {
      AssistantAvatar::drawUnderlay(_display.tft(), x, y, pixels, alpha);
      _underlayDrawn = true;
    }
  }
  const uint16_t *underlayPixels = nullptr;
  const uint8_t *underlayAlpha = nullptr;
  (void)_avatarLoader.underlay(underlayPixels, underlayAlpha);
  const uint16_t *avatarPixels = nullptr;
  const uint8_t *avatarAlpha = nullptr;
  (void)_avatarLoader.image(_avatarAsset, avatarPixels, avatarAlpha);
  _avatar.draw(_display.tft(), x, y, avatarPixels, ScreenBackground,
               avatarAlpha, underlayPixels);
  _lastAvatarRevision = _avatarLoader.revision();
}

void AssistantScreen::clearAvatarPane() {
  if (!_display.isReady()) return;
  const int16_t x = (_display.width() - AvatarWidth) / 2;
  const int16_t y = _display.height() - AvatarHeight;
  _avatar.clear(_display.tft(), x, y, ScreenBackground);
  _underlayDrawn = false;
}

void AssistantScreen::drawMicrophoneStrip() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, MicStripTop, _display.width(), MicStripHeight, UiKit::AppBarColor);
  const bool listening = _snapshot.phase == AssistantPhase::Listening;
  const char *label = !_snapshot.micAvailable ? "Mic unavailable" :
                      listening ? "Listening" : "Mic off";
  const uint16_t color = !_snapshot.micAvailable ? TFT_LIGHTGREY :
                         listening ? ListeningColor : TFT_WHITE;
  UiKit::drawMicrophoneStatusIcon(tft, 20, MicStripTop + MicStripHeight / 2,
                                  listening, _snapshot.micAvailable,
                                  color, UiKit::AppBarColor);
  UiText::draw(tft, label, 40, MicStripTop + MicStripHeight / 2,
               color, UiKit::AppBarColor, UiText::Size::Content, ML_DATUM);
  if (_snapshot.autoListen) {
    UiText::draw(tft, F("AUTO"), _display.width() - 12,
                 MicStripTop + MicStripHeight / 2, ListeningColor,
                 UiKit::AppBarColor, UiText::Size::Compact, MR_DATUM);
  }
}

uint16_t AssistantScreen::countTranscriptLines() const {
  uint16_t total = 0;
  String lines[MaxWrappedLines];
  const auto count = [&](const char *text) -> uint8_t {
    const String normalized = UiText::normalizeAssistantCaption(String(text ? text : ""));
    return wrapCaption(normalized, lines, MaxWrappedLines,
                       TranscriptTextWidth - TranscriptTextInset * 2);
  };
  AssistantTranscriptTurn turn = {};
  for (uint8_t index = 0; index < _snapshot.transcriptCount; ++index) {
    if (!_controller.copyTranscriptTurn(index, turn)) continue;
    total += count(turn.user);
    total += count(turn.assistant);
  }
  if (_controller.copyTranscriptDraft(turn)) {
    total += count(turn.user);
    total += count(turn.assistant);
  }
  if (total == 0) {
    if (_snapshot.phase == AssistantPhase::NeedsActivation) {
      total += count(_snapshot.activationMessage);
      total += count(_snapshot.activationCode);
    } else {
      total += count(_snapshot.status);
    }
  }
  return total;
}

void AssistantScreen::drawTranscriptMessage(const char *text, bool user,
                                             uint16_t &logicalLine,
                                             uint8_t &visibleLine) {
  if (!text || !text[0]) return;
  String lines[MaxWrappedLines];
  const String normalized = UiText::normalizeAssistantCaption(String(text));
  const uint8_t count = wrapCaption(normalized, lines, MaxWrappedLines,
                                    TranscriptTextWidth - TranscriptTextInset * 2);
  TFT_eSPI &tft = _display.tft();
  for (uint8_t index = 0; index < count; ++index, ++logicalLine) {
    if (logicalLine < _scrollLine || visibleLine >= VisibleTranscriptLines) continue;
    const int16_t measured = UiText::measureAssistantCaption(lines[index]);
    const int16_t width = min<int16_t>(TranscriptTextWidth - 8,
                                      measured + TranscriptTextInset * 2);
    const int16_t x = user ? TranscriptTextWidth - width - 4 : 4;
    const int16_t y = TranscriptTop + 4 + visibleLine * TranscriptLineHeight;
    const uint16_t fill = user ? UserBubbleColor : AssistantBubbleColor;
    tft.fillSmoothRoundRect(x, y, width, 21, 7, fill, ScreenBackground);
    UiText::drawAssistantCaption(tft, lines[index],
                                 user ? x + width - TranscriptTextInset
                                      : x + TranscriptTextInset,
                                 y + 10, TFT_WHITE, fill,
                                 user ? MR_DATUM : ML_DATUM);
    ++visibleLine;
  }
}

void AssistantScreen::drawTranscript(bool followTail) {
  if (!_display.isReady()) return;
  _totalLines = countTranscriptLines();
  const uint16_t bottom = _totalLines > VisibleTranscriptLines
      ? _totalLines - VisibleTranscriptLines : 0;
  if (followTail) _scrollLine = bottom;
  else if (_scrollLine > bottom) _scrollLine = bottom;

  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, TranscriptTop, _display.width(), TranscriptHeight,
               ScreenBackground);
  uint16_t logicalLine = 0;
  uint8_t visibleLine = 0;
  AssistantTranscriptTurn turn = {};
  for (uint8_t index = 0; index < _snapshot.transcriptCount; ++index) {
    if (!_controller.copyTranscriptTurn(index, turn)) continue;
    drawTranscriptMessage(turn.user, true, logicalLine, visibleLine);
    drawTranscriptMessage(turn.assistant, false, logicalLine, visibleLine);
  }
  if (_controller.copyTranscriptDraft(turn)) {
    drawTranscriptMessage(turn.user, true, logicalLine, visibleLine);
    drawTranscriptMessage(turn.assistant, false, logicalLine, visibleLine);
  }
  if (logicalLine == 0) {
    if (_snapshot.phase == AssistantPhase::NeedsActivation) {
      drawTranscriptMessage(_snapshot.activationMessage, false, logicalLine, visibleLine);
      drawTranscriptMessage(_snapshot.activationCode, false, logicalLine, visibleLine);
    } else {
      drawTranscriptMessage(_snapshot.status, false, logicalLine, visibleLine);
    }
  }
  tft.fillRect(TranscriptTextWidth, TranscriptTop, ScrollCellSize,
               TranscriptHeight, UiKit::AppBarColor);
  UiKit::drawPageArrow(tft, TranscriptTextWidth + ScrollCellSize / 2,
                       TranscriptTop + ScrollCellSize / 2, true, false,
                       _scrollLine ? TFT_WHITE : TFT_DARKGREY, UiKit::AppBarColor);
  UiKit::drawPageArrow(tft, TranscriptTextWidth + ScrollCellSize / 2,
                       TranscriptTop + TranscriptHeight - ScrollCellSize / 2,
                       true, true, _scrollLine < bottom ? TFT_WHITE : TFT_DARKGREY,
                       UiKit::AppBarColor);
}

void AssistantScreen::drawPrimaryAction() {
  if (!_display.isReady()) return;
  String label;
  bool enabled = !_snapshot.resettingConversation;
  if (_snapshot.resettingConversation) label = F("Cancel");
  else if (_snapshot.phase == AssistantPhase::Ready)
    label = _snapshot.transcriptCount ? F("Ask follow-up") : F("Ask");
  else if (_snapshot.phase == AssistantPhase::Listening) label = F("Stop listening");
  else if (active()) label = F("Cancel");
  else if (_snapshot.phase == AssistantPhase::NeedsWiFi) label = F("Wi-Fi settings");
  else if (_snapshot.phase == AssistantPhase::Error && _snapshot.retryable) label = F("Retry");
  else { label = F("Ask"); enabled = false; }
  UiKit::drawFilledButton(_display.tft(), _primaryActionBounds.x,
                          _primaryActionBounds.y, _primaryActionBounds.w,
                          _primaryActionBounds.h, label,
                          enabled ? PrimaryButtonColor : DisabledButtonColor,
                          enabled ? TFT_WHITE : TFT_LIGHTGREY, ScreenBackground);
}

void AssistantScreen::drawRootMenu() {
  if (!_display.isReady()) return;
  const int16_t x = _display.width() - PopupWidth - 8;
  for (uint8_t index = 0; index < 3; ++index) {
    _rootMenuRows[index] = {x, static_cast<int16_t>(TranscriptTop +
        index * UiKit::ListCellHeight), PopupWidth, UiKit::ListCellHeight};
  }
  UiKit::drawPopupMenuFrame(_display.tft(), x, TranscriptTop, PopupWidth,
                            UiKit::ListCellHeight * 3, ScreenBackground);
  UiKit::drawPopupMenuRow(_display.tft(), x, _rootMenuRows[0].y, PopupWidth,
      _confirmNewConversation ? F("Tap again to confirm") : F("New conversation"),
      _confirmNewConversation, ScreenBackground);
  UiKit::drawPopupMenuRow(_display.tft(), x, _rootMenuRows[1].y, PopupWidth,
                          F("Auto-listen"), _snapshot.autoListen, ScreenBackground);
  UiKit::drawPopupMenuRow(_display.tft(), x, _rootMenuRows[2].y, PopupWidth,
                          F("Avatar"), false, ScreenBackground);
  drawMicrophoneStrip();
}

void AssistantScreen::layoutAvatarMenu() {
  const int16_t width = min<int16_t>(PopupWidth, _display.width() - 16);
  const int16_t x = _display.width() - width - 8;
  const uint8_t count = _avatarLoader.avatarCount();
  _avatarMenuBounds = {x, TranscriptTop, width,
      static_cast<int16_t>(count * UiKit::ListCellHeight)};
  for (uint8_t index = 0; index < AssistantAvatarLoader::MaxAvatars; ++index) {
    _avatarMenuRows[index] = index < count
        ? Rect{x, static_cast<int16_t>(TranscriptTop + index * UiKit::ListCellHeight),
               width, UiKit::ListCellHeight}
        : Rect{0, 0, 0, 0};
  }
}

void AssistantScreen::drawAvatarMenu() {
  if (!_display.isReady()) return;
  layoutAvatarMenu();
  UiKit::drawPopupMenuFrame(_display.tft(), _avatarMenuBounds.x,
                            _avatarMenuBounds.y, _avatarMenuBounds.w,
                            _avatarMenuBounds.h, ScreenBackground);
  const uint8_t count = _avatarLoader.avatarCount();
  for (uint8_t index = 0; index < count; ++index) {
    const Rect &row = _avatarMenuRows[index];
    UiKit::drawPopupMenuRow(_display.tft(), row.x, row.y, row.w,
                            _avatarLoader.avatarName(index),
                            _avatarLoader.avatarSelected(index), ScreenBackground);
  }
  drawMicrophoneStrip();
}

void AssistantScreen::closeMenu() {
  if (_menu == Menu::Closed) return;
  _menu = Menu::Closed;
  _confirmNewConversation = false;
  draw(false);
}

bool AssistantScreen::active() const {
  return _snapshot.phase == AssistantPhase::Bootstrapping ||
         _snapshot.phase == AssistantPhase::NeedsActivation ||
         _snapshot.phase == AssistantPhase::Connecting ||
         _snapshot.phase == AssistantPhase::Listening ||
         _snapshot.phase == AssistantPhase::Thinking ||
         _snapshot.phase == AssistantPhase::Speaking;
}

void AssistantScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::Removed || event == SdCardEvent::MountFailed ||
      event == SdCardEvent::Mounted) {
    _avatarLoader.requestReload();
    clearAvatarPane();
    drawAvatar();
    drawPrimaryAction();
  }
}
