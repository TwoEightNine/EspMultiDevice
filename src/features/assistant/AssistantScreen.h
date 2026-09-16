#pragma once

#include "../../core/Screen.h"
#include "../../services/assistant/AssistantController.h"
#include "AssistantAvatar.h"

class AssistantScreen : public Screen {
public:
  AssistantScreen(Navigator &navigator, TouchController &touch,
                  DisplayController &display, AssistantController &controller);

  void onEnter() override;
  void onResume() override;
  void onExit() override;
  bool releaseResources() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static constexpr int16_t ButtonX = 152;
  static constexpr int16_t ButtonY = 274;
  static constexpr int16_t ButtonWidth = 176;
  static constexpr int16_t ButtonHeight = 42;
  static constexpr int16_t AvatarCenterY = 98;
  static constexpr int16_t TextTop = 149;
  static constexpr int16_t CaptionTop = 173;
  static constexpr uint8_t MaxCaptionLines = 6;

  AssistantController &_controller;
  AssistantAvatar _avatar;
  AssistantSnapshot _snapshot;
  bool _lastTouchDown;
  bool _closePending;
  bool _openingWiFi;
  uint32_t _lastAnimationMs;
  uint32_t _lastCloseRequestMs;

  void enter();
  void requestClose(bool openWiFi);
  void draw(bool full);
  void drawAvatar(uint32_t nowMs);
  void drawText();
  void drawButton();
  const char *buttonLabel() const;
  bool active() const;
  static bool avatarAnimated(AssistantPhase phase);
};
