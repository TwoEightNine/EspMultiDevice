#pragma once

#include "../../core/Screen.h"
#include "../../services/assistant/AssistantController.h"
#include "../../services/storage/SdCardController.h"
#include "AssistantAvatar.h"
#include "AssistantAvatarLoader.h"

class AssistantScreen : public Screen {
public:
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
  static constexpr int16_t TextLeft = 0;
  static constexpr int16_t TextTop = 48;
  static constexpr int16_t TextWidth = 319;
  static constexpr int16_t TextHeight = 192;
  static constexpr int16_t AvatarWidth = 320;
  static constexpr int16_t AvatarHeight = 240;
  static constexpr uint8_t MaxUserTextLines = 2;
  static constexpr uint8_t MaxAssistantTextLines = 4;
  static constexpr int16_t BubbleTextPadding = 12;

  struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };

  AssistantController &_controller;
  SdCardController &_sdCard;
  AssistantAvatar _avatar;
  AssistantAvatarLoader _avatarLoader;
  AssistantSnapshot _snapshot;
  bool _lastTouchDown;
  bool _closePending;
  bool _openingWiFi;
  uint32_t _lastCloseRequestMs;
  uint32_t _lastAvatarRevision;
  AssistantAvatar::Asset _avatarAsset;
  bool _avatarAssetValid;
  bool _underlayDrawn;
  bool _avatarMenuOpen;
  Rect _avatarMenuBounds;
  Rect _avatarMenuRows[AssistantAvatarLoader::MaxAvatars];
  uint32_t _lastAvatarCatalogRevision;

  void enter(bool resetAvatarSelection);
  void requestClose(bool openWiFi);
  void draw(bool full);
  void drawAvatar();
  void clearAvatarPane();
  void drawText();
  void layoutAvatarMenu();
  void drawAvatarMenu();
  void closeAvatarMenu();
  bool active() const;
  void handleSdCardEvent(SdCardEvent event);
};
