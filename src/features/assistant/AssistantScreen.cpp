#include "AssistantScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"

#include <string.h>

namespace {
constexpr uint16_t LightErrorColor = 0xF986;  // soft red on black
constexpr uint16_t BubbleBorderColor = TFT_WHITE;
constexpr int16_t BubbleRadius = 8;

constexpr int16_t UserBubbleX = 62;
constexpr int16_t UserBubbleY = 58;
constexpr int16_t UserBubbleWidth = 246;
constexpr int16_t UserBubbleHeight = 55;
constexpr int16_t UserTextTop = 66;

constexpr int16_t AssistantBubbleX = 12;
constexpr int16_t AssistantBubbleY = 125;
constexpr int16_t AssistantBubbleWidth = 246;
constexpr int16_t AssistantBubbleHeight = 103;
constexpr int16_t AssistantTextTop = 134;
constexpr int16_t AvatarMenuTop = UiKit::AppBarHeight + 8;
constexpr int16_t AvatarMenuWidth = 224;

void drawSpeechBubble(TFT_eSPI &tft, int16_t x, int16_t y,
                      int16_t width, int16_t height, bool tailPointsUp) {
  // The V stays within the text viewport: the upper tip is y=50 and the
  // lower tip is y=237. Draw the frame after the tail so the joins remain
  // crisp at the bubble edge.
  if (tailPointsUp) {
    tft.drawWideLine(x + width - 29, y, x + width - 19, y - 8,
                     2.0f, BubbleBorderColor, TFT_BLACK);
    tft.drawWideLine(x + width - 19, y - 8, x + width - 9, y,
                     2.0f, BubbleBorderColor, TFT_BLACK);
  } else {
    tft.drawWideLine(x + 12, y + height - 1, x + 22, y + height + 9,
                     2.0f, BubbleBorderColor, TFT_BLACK);
    tft.drawWideLine(x + 22, y + height + 9, x + 32, y + height - 1,
                     2.0f, BubbleBorderColor, TFT_BLACK);
  }
  tft.fillSmoothRoundRect(x, y, width, height, BubbleRadius,
                          TFT_BLACK, TFT_BLACK);
  tft.drawSmoothRoundRect(x, y, BubbleRadius, BubbleRadius, width, height,
                          BubbleBorderColor, TFT_BLACK);
}

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
    count++;
  }
  if (position < text.length() && count > 0) {
    String remainder = lines[count - 1] + " " + text.substring(position);
    remainder.trim();
    lines[count - 1] = UiText::ellipsizeAssistantCaption(remainder, maxWidth);
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
    : Screen(navigator, touch, display), _controller(controller), _sdCard(sdCard), _avatar(),
      _avatarLoader(sdCard), _snapshot{}, _lastTouchDown(false),
      _closePending(false), _openingWiFi(false), _lastCloseRequestMs(0),
      _lastAvatarRevision(0), _avatarAsset(AssistantAvatar::Asset::Neutral),
      _avatarAssetValid(false), _underlayDrawn(false), _avatarMenuOpen(false),
      _avatarMenuBounds{0, 0, 0, 0}, _avatarMenuRows{},
      _lastAvatarCatalogRevision(0) {
  _snapshot.clear();
}

void AssistantScreen::onEnter() { enter(true); }
void AssistantScreen::onResume() { enter(false); }

void AssistantScreen::enter(bool resetAvatarSelection) {
  _lastTouchDown = false;
  _closePending = false;
  _openingWiFi = false;
  _lastCloseRequestMs = 0;
  _avatarLoader.begin();
  if (resetAvatarSelection) _avatarLoader.resetAvatar();
  _controller.activate();
  _snapshot = _controller.snapshot();
  _avatarAsset = AssistantAvatar::assetFor(_snapshot.phase, _snapshot.emotion);
  _avatarAssetValid = true;
  _underlayDrawn = false;
  _avatarMenuOpen = false;
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

void AssistantScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown || _closePending) return;
  _lastTouchDown = true;
  if (_avatarMenuOpen) {
    const uint8_t count = _avatarLoader.avatarCount();
    for (uint8_t index = 0; index < count; ++index) {
      const Rect &row = _avatarMenuRows[index];
      if (point.x >= row.x && point.x < row.x + row.w &&
          point.y >= row.y && point.y < row.y + row.h) {
        (void)_avatarLoader.selectAvatar(index);
        closeAvatarMenu();
        return;
      }
    }
    closeAvatarMenu();
    return;
  }
  if (UiKit::navigationIconContains(point.x, point.y)) {
    requestClose(false);
    return;
  }
  if (UiKit::appBarActionContains(point.x, point.y, _display.width())) {
    _avatarMenuOpen = true;
    drawAvatarMenu();
    return;
  }
  const int16_t avatarX = (_display.width() - AvatarWidth) / 2;
  const int16_t avatarY = _display.height() - AvatarHeight;
  if (point.x < avatarX || point.x >= avatarX + AvatarWidth ||
      point.y < avatarY || point.y >= avatarY + AvatarHeight) return;
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
  // A tap from Ready enters Connecting before the first turn audio arrives.
  // Keep the face that was already on screen during this short handshake;
  // later phases are free to select their normal artwork.
  AssistantAvatar::Asset nextAsset =
      AssistantAvatar::assetFor(next.phase, next.emotion);
  if (_avatarAssetValid && next.phase == AssistantPhase::Connecting &&
      (_snapshot.phase == AssistantPhase::Ready ||
       _snapshot.phase == AssistantPhase::Connecting)) {
    nextAsset = _avatarAsset;
  }
  const bool avatarChanged = !_avatarAssetValid || nextAsset != _avatarAsset ||
                             _avatarLoader.revision() != _lastAvatarRevision;
  const bool catalogChanged = _avatarLoader.catalogRevision() != _lastAvatarCatalogRevision;
  const bool textChanged = next.phase != _snapshot.phase ||
                           !sameText(next.status, _snapshot.status) ||
                           !sameText(next.activationMessage, _snapshot.activationMessage) ||
                           !sameText(next.activationCode, _snapshot.activationCode) ||
                           !sameText(next.sttCaption, _snapshot.sttCaption) ||
                           !sameText(next.ttsCaption, _snapshot.ttsCaption);
  _snapshot = next;
  _avatarAsset = nextAsset;
  _avatarAssetValid = true;
  if (avatarChanged) drawAvatar();
  if (textChanged) drawText();
  if (_avatarMenuOpen && (avatarChanged || textChanged || catalogChanged)) drawAvatarMenu();
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
  TFT_eSPI &tft = _display.tft();
  if (full) {
    UiKit::drawTopAppBar(tft, _display.width(), F("Assistant"),
                         UiKit::NavigationIcon::Close,
                         UiKit::AppBarAction::More);
    clearAvatarPane();
  }
  drawText();
  drawAvatar();
  if (_avatarMenuOpen) drawAvatarMenu();
}

void AssistantScreen::drawAvatar() {
  if (!_display.isReady()) return;
  const int16_t x = (_display.width() - AvatarWidth) / 2;
  const int16_t y = _display.height() - AvatarHeight;
  if (!_underlayDrawn) {
    const uint16_t *underlayPixels = nullptr;
    const uint8_t *underlayAlpha = nullptr;
    if (_avatarLoader.underlay(underlayPixels, underlayAlpha)) {
      AssistantAvatar::drawUnderlay(_display.tft(), x, y,
                                    underlayPixels, underlayAlpha);
      _underlayDrawn = true;
    }
  }
  const uint16_t *underlayPixels = nullptr;
  const uint8_t *underlayAlpha = nullptr;
  (void)_avatarLoader.underlay(underlayPixels, underlayAlpha);
  (void)underlayAlpha;
  const AssistantAvatar::Asset asset = _avatarAssetValid
      ? _avatarAsset
      : AssistantAvatar::assetFor(_snapshot.phase, _snapshot.emotion);
  // Push replacement artwork directly over the old frame to avoid a blink.
  const uint16_t *avatarPixels = nullptr;
  const uint8_t *avatarAlpha = nullptr;
  (void)_avatarLoader.image(asset, avatarPixels, avatarAlpha);
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

void AssistantScreen::drawText() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(TextLeft, TextTop, TextWidth, TextHeight, ScreenBackground);
  drawSpeechBubble(tft, UserBubbleX, UserBubbleY,
                   UserBubbleWidth, UserBubbleHeight, true);
  drawSpeechBubble(tft, AssistantBubbleX, AssistantBubbleY,
                   AssistantBubbleWidth, AssistantBubbleHeight, false);

  String userCaption = UiText::normalizeAssistantCaption(String(_snapshot.sttCaption));
  String userLines[MaxUserTextLines];
  const uint8_t userCount = wrapCaption(userCaption, userLines, MaxUserTextLines,
                                        UserBubbleWidth - BubbleTextPadding * 2);
  for (uint8_t index = 0; index < userCount; ++index) {
    UiText::drawAssistantCaption(tft, userLines[index],
                                 UserBubbleX + BubbleTextPadding,
                                 UserTextTop + index * UiText::lineHeight(UiText::Size::Content),
                                 TFT_WHITE, ScreenBackground, TL_DATUM);
  }

  String assistantCaption;
  uint16_t assistantColor = _snapshot.phase == AssistantPhase::Error
      ? LightErrorColor : TFT_LIGHTGREY;
  if (_snapshot.phase == AssistantPhase::NeedsActivation) {
    assistantCaption = String(_snapshot.activationMessage);
    if (_snapshot.activationCode[0]) assistantCaption += "\n" + String(_snapshot.activationCode);
  } else if (_snapshot.ttsCaption[0]) {
    assistantCaption = String(_snapshot.ttsCaption);
    assistantColor = TFT_WHITE;
  } else {
    assistantCaption = String(_snapshot.status);
  }
  assistantCaption = UiText::normalizeAssistantCaption(assistantCaption);
  String assistantLines[MaxAssistantTextLines];
  const uint8_t assistantCount = wrapCaption(assistantCaption, assistantLines,
                                             MaxAssistantTextLines,
                                             AssistantBubbleWidth - BubbleTextPadding * 2);
  for (uint8_t index = 0; index < assistantCount; ++index) {
    UiText::drawAssistantCaption(tft, assistantLines[index],
                                 AssistantBubbleX + BubbleTextPadding,
                                 AssistantTextTop + index * UiText::lineHeight(UiText::Size::Content),
                                 assistantColor, ScreenBackground, TL_DATUM);
  }
}

void AssistantScreen::layoutAvatarMenu() {
  const int16_t width = min<int16_t>(AvatarMenuWidth, _display.width() - 16);
  const int16_t x = _display.width() - width - 8;
  const uint8_t count = _avatarLoader.avatarCount();
  _avatarMenuBounds = {x, AvatarMenuTop, width,
                        static_cast<int16_t>(count * UiKit::ListCellHeight)};
  for (uint8_t index = 0; index < AssistantAvatarLoader::MaxAvatars; ++index) {
    _avatarMenuRows[index] = index < count
        ? Rect{x, static_cast<int16_t>(AvatarMenuTop + index * UiKit::ListCellHeight),
               width, UiKit::ListCellHeight}
        : Rect{0, 0, 0, 0};
  }
}

void AssistantScreen::drawAvatarMenu() {
  if (!_display.isReady()) return;
  layoutAvatarMenu();
  TFT_eSPI &tft = _display.tft();
  UiKit::drawPopupMenuFrame(tft, _avatarMenuBounds.x, _avatarMenuBounds.y,
                            _avatarMenuBounds.w, _avatarMenuBounds.h,
                            ScreenBackground);
  const uint8_t count = _avatarLoader.avatarCount();
  for (uint8_t index = 0; index < count; ++index) {
    const Rect &row = _avatarMenuRows[index];
    UiKit::drawPopupMenuRow(tft, row.x, row.y, row.w,
                            _avatarLoader.avatarName(index),
                            _avatarLoader.avatarSelected(index),
                            ScreenBackground);
  }
}

void AssistantScreen::closeAvatarMenu() {
  if (!_avatarMenuOpen) return;
  _avatarMenuOpen = false;
  // The popup can cover both caption bubbles and the avatar pane. A full UI
  // redraw restores them from the current snapshot without touching the
  // assistant controller or its active dialogue.
  draw(true);
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
  }
}
