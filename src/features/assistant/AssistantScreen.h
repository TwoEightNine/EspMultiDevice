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
  static constexpr int16_t MicStripTop = 48;
  static constexpr int16_t MicStripHeight = 32;
  static constexpr int16_t TranscriptTop = 80;
  static constexpr int16_t TranscriptHeight = 160;
  static constexpr int16_t TranscriptTextWidth = 280;
  static constexpr int16_t ScrollCellSize = 40;
  static constexpr int16_t AvatarWidth = 320;
  static constexpr int16_t AvatarHeight = 240;
  static constexpr uint8_t VisibleTranscriptLines = 6;
  static constexpr uint8_t MaxWrappedLines = 24;

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
  AssistantSnapshot _snapshot;
  bool _lastTouchDown;
  bool _closePending;
  bool _openingWiFi;
  uint32_t _lastCloseRequestMs;
  uint32_t _lastAvatarRevision;
  AssistantAvatar::Asset _avatarAsset;
  bool _avatarAssetValid;
  bool _underlayDrawn;
  Menu _menu;
  bool _confirmNewConversation;
  Rect _avatarMenuBounds;
  Rect _avatarMenuRows[AssistantAvatarLoader::MaxAvatars];
  uint32_t _lastAvatarCatalogRevision;
  uint16_t _scrollLine;
  uint16_t _totalLines;
  Rect _scrollUpBounds;
  Rect _scrollDownBounds;
  Rect _primaryActionBounds;
  Rect _rootMenuRows[3];

  void enter(bool resetAvatarSelection);
  void requestClose(bool openWiFi);
  void draw(bool full);
  void drawAvatar();
  void clearAvatarPane();
  void drawMicrophoneStrip();
  void drawTranscript(bool preservePosition = true);
  void drawPrimaryAction();
  uint16_t countTranscriptLines() const;
  void drawTranscriptMessage(const char *text, bool user, uint16_t &logicalLine,
                             uint8_t &visibleLine);
  void layoutAvatarMenu();
  void drawRootMenu();
  void drawAvatarMenu();
  void closeMenu();
  bool contains(const Rect &rect, int16_t x, int16_t y) const;
  bool active() const;
  void handleSdCardEvent(SdCardEvent event);
};
