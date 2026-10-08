#pragma once

#include <Arduino.h>
#include <USBHIDConsumerControl.h>
#include <USBHIDKeyboard.h>

enum class MacRemoteState : uint8_t {
  NotStarted,
  WaitingForHost,
  Connected,
  Suspended,
  Error
};

enum class MacRemoteAction : uint8_t {
  YouTube,
  Screenshot,
  Lock,
  Previous,
  PlayPause,
  Next,
  Mute,
  VolumeDown,
  VolumeUp
};

class MacRemoteController {
public:
  MacRemoteController();

  bool begin();
  void tick(uint32_t nowMs);
  MacRemoteState state() const;
  bool hasConnected() const;
  bool busy() const;
  bool send(MacRemoteAction action);
  void releaseAll();

private:
  USBHIDKeyboard _keyboard;
  USBHIDConsumerControl _consumerControl;
  mutable portMUX_TYPE _stateMux;
  MacRemoteState _state;
  bool _started;
  bool _hasConnected;
  enum class MacroStep : uint8_t {
    Idle,
    WaitForSpotlight,
    TypeUrl,
    WaitForResult,
    WaitForSelection
  };
  MacroStep _macroStep;
  uint32_t _macroAtMs;
  size_t _macroIndex;
  char _macroUrl[96];

  static MacRemoteController *_instance;
  static void onUsbEvent(void *arg, esp_event_base_t eventBase,
                         int32_t eventId, void *eventData);

  void setState(MacRemoteState state);
  bool startUrlMacro(const char *url, uint32_t nowMs);
  void cancelMacro();
  bool sendKeyboardShortcut(uint8_t firstModifier,
                            uint8_t secondModifier,
                            uint8_t key);
  bool sendConsumerControl(uint16_t usage);
};
