#include "AssistantScreen.h"

#include "AssistantCaptionLayout.h"
#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"

#include <string.h>

namespace {
constexpr uint16_t AwaitingColor = 0x249F;
constexpr uint16_t ListeningColor = 0x07E0;
constexpr uint16_t UnavailableColor = 0x4208;
constexpr uint16_t AutoListenPulseColor = TFT_CYAN;
constexpr int16_t CaptionInsetX = 8;
constexpr int16_t CaptionInsetTop = 3;
constexpr int16_t PopupWidth = 260;

bool sameText(const char *left, const char *right) {
  return strcmp(left ? left : "", right ? right : "") == 0;
}
}  // namespace

AssistantScreen::AssistantScreen(Navigator &navigator, TouchController &touch,
                                 DisplayController &display,
                                 AssistantController &controller,
                                 SdCardController &sdCard)
    : Screen(navigator, touch, display), _controller(controller), _sdCard(sdCard),
      _avatar(), _avatarLoader(sdCard), _voiceGesture(), _voicePulseVisible(false),
      _snapshot{}, _lastTouchDown(false),
      _closePending(false), _openingWiFi(false), _lastCloseRequestMs(0),
      _lastAvatarRevision(0), _lastBackgroundRevision(0),
      _avatarAsset(AssistantAvatar::Asset::Neutral), _avatarAssetValid(false),
      _menu(Menu::Closed), _confirmNewConversation(false),
      _avatarMenuBounds{0, 0, 0, 0}, _avatarMenuRows{},
      _lastAvatarCatalogRevision(0), _humanBubbleBounds{
          AssistantCaptionLayout::HumanX, AssistantCaptionLayout::HumanY,
          AssistantCaptionLayout::HumanWidth, AssistantCaptionLayout::HumanHeight},
      _assistantBubbleBounds{
          AssistantCaptionLayout::AssistantX, AssistantCaptionLayout::AssistantY,
          AssistantCaptionLayout::AssistantWidth, AssistantCaptionLayout::AssistantHeight},
      _primaryActionBounds{AssistantCaptionLayout::VoiceControlX,
          AssistantCaptionLayout::VoiceControlY,
          AssistantCaptionLayout::VoiceControlSize,
          AssistantCaptionLayout::VoiceControlSize}, _rootMenuRows{},
      _assistantReplyStarted(false), _assistantReplyDraft(false),
      _assistantReplyText(), _assistantReplyStartedMs(0), _assistantPageStart(0) {
  _snapshot.clear();
}

void AssistantScreen::onEnter() { enter(true); }
void AssistantScreen::onResume() { enter(false); }

void AssistantScreen::enter(bool resetAvatarSelection) {
  _lastTouchDown = false;
  _voiceGesture.reset();
  _voicePulseVisible = false;
  _closePending = false;
  _openingWiFi = false;
  _lastCloseRequestMs = 0;
  _menu = Menu::Closed;
  _confirmNewConversation = false;
  _assistantReplyStarted = false;
  _assistantReplyDraft = false;
  _assistantReplyText = "";
  _assistantReplyStartedMs = 0;
  _assistantPageStart = 0;
  _avatarLoader.begin();
  if (resetAvatarSelection) _avatarLoader.resetAvatar();
  _controller.activate();
  _snapshot = _controller.snapshot();
  _avatarAsset = AssistantAvatar::assetFor(_snapshot.phase, _snapshot.emotion);
  _avatarAssetValid = true;
  _lastAvatarRevision = _avatarLoader.revision();
  _lastBackgroundRevision = _avatarLoader.backgroundRevision();
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
  if (!point.touched) {
    _lastTouchDown = false;
    _voiceGesture.release();
    return;
  }
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
  if (!contains(_primaryActionBounds, point.x, point.y) ||
      _snapshot.resettingConversation ||
      (_snapshot.phase == AssistantPhase::Ready && !_snapshot.micAvailable)) return;
  const bool canArmAutoListen = _snapshot.autoListen ||
      _snapshot.phase == AssistantPhase::Ready || active();
  const AssistantVoiceGesture::Event gesture = canArmAutoListen
      ? _voiceGesture.press(millis(), _snapshot.autoListen)
      : AssistantVoiceGesture::Event::None;
  if (gesture == AssistantVoiceGesture::Event::DisableAutoListen) {
    _controller.setAutoListen(false);
    return;
  }
  if (_snapshot.phase == AssistantPhase::Ready) _controller.startTurn();
  else if (active()) _controller.cancelTurn();
  else if (_snapshot.phase == AssistantPhase::NeedsWiFi) requestClose(true);
  else if (_snapshot.phase == AssistantPhase::Error && _snapshot.retryable)
    _controller.retry();
}

void AssistantScreen::tick(uint32_t nowMs) {
  handleSdCardEvent(_sdCard.takeEvent());
  _controller.tick(nowMs);
  AssistantSnapshot next = _controller.snapshot();
  bool pulseChanged = false;
  const TouchPoint heldPoint = _touch.point();
  if (_voiceGesture.tick(nowMs, _touch.isTouched() &&
                         contains(_primaryActionBounds, heldPoint.x, heldPoint.y)) ==
      AssistantVoiceGesture::Event::EnableAutoListen) {
    _controller.setAutoListen(true);
    next = _controller.snapshot();
    _voicePulseVisible = true;
    pulseChanged = true;
  }
  if (_voicePulseVisible && !_voiceGesture.pulsing(nowMs)) {
    _voicePulseVisible = false;
    pulseChanged = true;
  }
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
  const bool backgroundChanged =
      _avatarLoader.backgroundRevision() != _lastBackgroundRevision;
  const bool catalogChanged =
      _avatarLoader.catalogRevision() != _lastAvatarCatalogRevision;
  const bool statusChanged = next.phase != _snapshot.phase ||
      next.micAvailable != _snapshot.micAvailable ||
      next.resettingConversation != _snapshot.resettingConversation ||
      !sameText(next.status, _snapshot.status) ||
      !sameText(next.activationMessage, _snapshot.activationMessage) ||
      !sameText(next.activationCode, _snapshot.activationCode);
  const bool transcriptChanged =
      next.transcriptRevision != _snapshot.transcriptRevision;
  _snapshot = next;
  _avatarAsset = nextAsset;
  _avatarAssetValid = true;

  if (backgroundChanged) {
    draw(true);
  } else {
    if (avatarChanged || statusChanged || pulseChanged) {
      drawAvatar();
      drawPrimaryAction();
    }
    if (transcriptChanged || statusChanged) drawCaptions(nowMs);
  }
  if (_assistantReplyStarted &&
      AssistantCaptionLayout::assistantPageStart(
          AssistantCaptionLayout::countLines(_assistantReplyText),
          _assistantReplyStartedMs, nowMs) != _assistantPageStart) {
    drawCaptions(nowMs);
  }
  if (_menu == Menu::Root && (statusChanged || transcriptChanged || backgroundChanged))
    drawRootMenu();
  if (_menu == Menu::Avatars && (avatarChanged || statusChanged ||
                                  transcriptChanged || backgroundChanged || catalogChanged))
    drawAvatarMenu();
  _lastAvatarRevision = _avatarLoader.revision();
  _lastBackgroundRevision = _avatarLoader.backgroundRevision();
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
  }
  drawBackground();
  drawCaptions(millis());
  drawAvatar();
  drawPrimaryAction();
  if (_menu == Menu::Root) drawRootMenu();
  else if (_menu == Menu::Avatars) drawAvatarMenu();
}

void AssistantScreen::drawBackground() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  const uint16_t *pixels = nullptr;
  if (_display.width() == AssistantAvatarLoader::BackgroundWidth &&
      _avatarLoader.background(pixels)) {
    tft.pushImage(0, BackgroundTop, AssistantAvatarLoader::BackgroundWidth,
                  AssistantAvatarLoader::BackgroundHeight, pixels);
  } else {
    tft.fillRect(0, BackgroundTop, _display.width(), _display.height() - BackgroundTop,
                 ScreenBackground);
  }
}

void AssistantScreen::restoreBackground(const Rect &rect) {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  const uint16_t *pixels = nullptr;
  if (_display.width() != AssistantAvatarLoader::BackgroundWidth ||
      !_avatarLoader.background(pixels)) {
    tft.fillRect(rect.x, rect.y, rect.w, rect.h, ScreenBackground);
    return;
  }
  const int16_t sourceY = rect.y - BackgroundTop;
  if (rect.x < 0 || sourceY < 0 || rect.x + rect.w > AssistantAvatarLoader::BackgroundWidth ||
      sourceY + rect.h > AssistantAvatarLoader::BackgroundHeight) {
    tft.fillRect(rect.x, rect.y, rect.w, rect.h, ScreenBackground);
    return;
  }
  for (int16_t row = 0; row < rect.h; ++row) {
    const uint16_t *source = pixels +
        static_cast<size_t>(sourceY + row) * AssistantAvatarLoader::BackgroundWidth + rect.x;
    tft.pushImage(rect.x, rect.y + row, rect.w, 1, source);
  }
}

void AssistantScreen::drawAvatar() {
  if (!_display.isReady()) return;
  const Rect avatarBounds{0, AvatarTop, AvatarWidth, AvatarHeight};
  restoreBackground(avatarBounds);
  const uint16_t *background = nullptr;
  if (_avatarLoader.background(background)) {
    background += static_cast<size_t>(AvatarTop - BackgroundTop) *
        AssistantAvatarLoader::BackgroundWidth;
  }
  const uint16_t *avatarPixels = nullptr;
  const uint8_t *avatarAlpha = nullptr;
  (void)_avatarLoader.image(_avatarAsset, avatarPixels, avatarAlpha);
  _avatar.draw(_display.tft(), 0, AvatarTop, avatarPixels, ScreenBackground,
               avatarAlpha, background);
}

bool AssistantScreen::latestTurn(AssistantTranscriptTurn &turn, bool &draft) const {
  draft = _controller.copyTranscriptDraft(turn);
  if (draft) return true;
  if (_snapshot.transcriptCount == 0) return false;
  return _controller.copyTranscriptTurn(_snapshot.transcriptCount - 1, turn);
}

void AssistantScreen::drawCaptionLines(const String *lines, uint8_t count,
                                       const Rect &bounds) {
  if (!lines) return;
  const int16_t lineHeight = UiText::lineHeight(UiText::Size::Content);
  for (uint8_t index = 0; index < count; ++index) {
    UiText::drawAssistantCaption(_display.tft(), lines[index],
                                 bounds.x + CaptionInsetX,
                                 bounds.y + CaptionInsetTop + index * lineHeight,
                                 TFT_WHITE, ScreenBackground, TL_DATUM);
  }
}

void AssistantScreen::drawCaptions(uint32_t nowMs) {
  if (!_display.isReady()) return;
  restoreBackground(_humanBubbleBounds);
  restoreBackground(_assistantBubbleBounds);

  AssistantTranscriptTurn turn = {};
  bool draft = false;
  const bool hasTurn = latestTurn(turn, draft);
  const String human = hasTurn ? String(turn.user) : String();
  String assistant = hasTurn ? String(turn.assistant) : String();
  const bool spokenReply = hasTurn && assistant.length() > 0;
  if (!spokenReply) {
    assistant = _snapshot.phase == AssistantPhase::NeedsActivation
        ? String(_snapshot.activationMessage) : String(_snapshot.status);
  }

  String humanLines[AssistantCaptionLayout::HumanLineCapacity];
  const uint8_t humanCount = AssistantCaptionLayout::copyHuman(human, humanLines);
  drawCaptionLines(humanLines, humanCount, _humanBubbleBounds);

  if (!spokenReply) {
    _assistantReplyStarted = false;
    _assistantReplyDraft = false;
    _assistantReplyText = "";
    _assistantReplyStartedMs = 0;
    _assistantPageStart = 0;
    String lines[AssistantCaptionLayout::AssistantLineCapacity];
    const uint8_t count = AssistantCaptionLayout::copyWindow(
        assistant, 0, lines, AssistantCaptionLayout::AssistantLineCapacity);
    drawCaptionLines(lines, count, _assistantBubbleBounds);
    return;
  }

  const bool newReply = !_assistantReplyStarted ||
      (draft && !_assistantReplyDraft) ||
      !assistant.startsWith(_assistantReplyText);
  if (newReply) {
    _assistantReplyStarted = true;
    _assistantReplyStartedMs = nowMs;
    _assistantPageStart = 0;
  }
  _assistantReplyDraft = draft;
  _assistantReplyText = assistant;
  const uint16_t lineCount = AssistantCaptionLayout::countLines(assistant);
  _assistantPageStart = AssistantCaptionLayout::assistantPageStart(
      lineCount, _assistantReplyStartedMs, nowMs);
  String lines[AssistantCaptionLayout::AssistantLineCapacity];
  const uint8_t count = AssistantCaptionLayout::copyWindow(
      assistant, _assistantPageStart, lines,
      AssistantCaptionLayout::AssistantLineCapacity);
  drawCaptionLines(lines, count, _assistantBubbleBounds);
}

void AssistantScreen::drawPrimaryAction() {
  if (!_display.isReady()) return;
  const AssistantCaptionLayout::VoiceAction action =
      AssistantCaptionLayout::voiceAction(static_cast<uint8_t>(_snapshot.phase), _snapshot.micAvailable,
                                           _snapshot.resettingConversation);
  UiKit::VoiceActionIcon icon = UiKit::VoiceActionIcon::Microphone;
  uint16_t fill = AwaitingColor;
  uint16_t foreground = TFT_WHITE;
  switch (action) {
    case AssistantCaptionLayout::VoiceAction::Stop:
      icon = UiKit::VoiceActionIcon::Stop;
      fill = ListeningColor;
      break;
    case AssistantCaptionLayout::VoiceAction::Cancel:
      icon = UiKit::VoiceActionIcon::Cancel;
      break;
    case AssistantCaptionLayout::VoiceAction::Unavailable:
      icon = UiKit::VoiceActionIcon::Unavailable;
      fill = UnavailableColor;
      foreground = TFT_LIGHTGREY;
      break;
    case AssistantCaptionLayout::VoiceAction::Microphone:
    default:
      break;
  }
  if (_voiceGesture.pulsing(millis())) fill = AutoListenPulseColor;
  const int16_t centerX = _primaryActionBounds.x + _primaryActionBounds.w / 2;
  const int16_t centerY = _primaryActionBounds.y + _primaryActionBounds.h / 2;
  _display.tft().fillSmoothCircle(centerX, centerY, _primaryActionBounds.w / 2,
                                  fill, ScreenBackground);
  UiKit::drawVoiceActionIcon(_display.tft(), centerX, centerY, icon, foreground, fill);
}

void AssistantScreen::drawRootMenu() {
  if (!_display.isReady()) return;
  const int16_t x = _display.width() - PopupWidth - 8;
  for (uint8_t index = 0; index < RootMenuRowCount; ++index) {
    _rootMenuRows[index] = {x, static_cast<int16_t>(BackgroundTop +
        index * UiKit::ListCellHeight), PopupWidth, UiKit::ListCellHeight};
  }
  UiKit::drawPopupMenuFrame(_display.tft(), x, BackgroundTop, PopupWidth,
                            UiKit::ListCellHeight * RootMenuRowCount, ScreenBackground);
  UiKit::drawPopupMenuRow(_display.tft(), x, _rootMenuRows[0].y, PopupWidth,
      _confirmNewConversation ? F("Tap again to confirm") : F("New conversation"),
      _confirmNewConversation, ScreenBackground);
  UiKit::drawPopupMenuRow(_display.tft(), x, _rootMenuRows[1].y, PopupWidth,
                          F("Avatar"), false, ScreenBackground);
}

void AssistantScreen::layoutAvatarMenu() {
  const int16_t width = min<int16_t>(PopupWidth, _display.width() - 16);
  const int16_t x = _display.width() - width - 8;
  const uint8_t count = _avatarLoader.avatarCount();
  _avatarMenuBounds = {x, BackgroundTop, width,
      static_cast<int16_t>(count * UiKit::ListCellHeight)};
  for (uint8_t index = 0; index < AssistantAvatarLoader::MaxAvatars; ++index) {
    _avatarMenuRows[index] = index < count
        ? Rect{x, static_cast<int16_t>(BackgroundTop + index * UiKit::ListCellHeight),
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
    draw(true);
  }
}
