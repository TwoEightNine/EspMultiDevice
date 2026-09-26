#pragma once

#include "../../core/Screen.h"
#include "../../services/assistant/AssistantController.h"
#include "../../services/storage/SdCardController.h"
#include "AssistantAvatar.h"
#include "AssistantAvatarLoader.h"
#include "AssistantVoiceGesture.h"

class AssistantScreen : public Screen {
public:
  // Kept public for the compile-time Assistant UI contract.
  static constexpr uint8_t RootMenuRowCount = 2;

  AssistantScreen(Navigator &navigator, TouchController &touch,
                  DisplayController &display, AssistantController &controller,
                  SdCardController &sdCard);

  void onEnter() override;
  void onResume() override;
  void onExit() override;
  bool releaseResources() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static constexpr uint16_t ScreenBackground = TFT_BLACK;
  static constexpr int16_t BackgroundTop = 48;
  static constexpr int16_t AvatarTop = 240;
  static constexpr int16_t AvatarWidth = 320;
  static constexpr int16_t AvatarHeight = 240;

  struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };

  enum class Menu : uint8_t { Closed, Root, Avatars };

  AssistantController &_controller;
  SdCardController &_sdCard;
  AssistantAvatar _avatar;
  AssistantAvatarLoader _avatarLoader;
  AssistantVoiceGesture _voiceGesture;
  bool _voicePulseVisible;
  AssistantSnapshot _snapshot;
  bool _lastTouchDown;
  bool _closePending;
  bool _openingWiFi;
  uint32_t _lastCloseRequestMs;
  uint32_t _lastAvatarRevision;
  uint32_t _lastBackgroundRevision;
  AssistantAvatar::Asset _avatarAsset;
  bool _avatarAssetValid;
  Menu _menu;
  bool _confirmNewConversation;
  Rect _avatarMenuBounds;
  Rect _avatarMenuRows[AssistantAvatarLoader::MaxAvatars];
  uint32_t _lastAvatarCatalogRevision;
  Rect _humanBubbleBounds;
  Rect _assistantBubbleBounds;
  Rect _primaryActionBounds;
  Rect _rootMenuRows[RootMenuRowCount];
  bool _assistantReplyStarted;
  bool _assistantReplyDraft;
  String _assistantReplyText;
  uint32_t _assistantReplyStartedMs;
  uint16_t _assistantPageStart;

  void enter(bool resetAvatarSelection);
  void requestClose(bool openWiFi);
  void draw(bool full);
  void drawBackground();
  void restoreBackground(const Rect &rect);
  void drawAvatar();
  void drawCaptions(uint32_t nowMs);
  void drawCaptionLines(const String *lines, uint8_t count, const Rect &bounds);
  bool latestTurn(AssistantTranscriptTurn &turn, bool &draft) const;
  void drawPrimaryAction();
  void layoutAvatarMenu();
  void drawRootMenu();
  void drawAvatarMenu();
  void closeMenu();
  bool contains(const Rect &rect, int16_t x, int16_t y) const;
  bool active() const;
  void handleSdCardEvent(SdCardEvent event);
};
