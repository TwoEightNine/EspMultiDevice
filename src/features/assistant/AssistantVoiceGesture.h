#pragma once

#include <Arduino.h>

// Converts the Assistant voice-control press lifecycle into bounded, testable
// events. The screen owns hit testing and performs the controller commands.
class AssistantVoiceGesture {
public:
  static constexpr uint32_t AutoListenHoldMs = 800;
  static constexpr uint32_t AutoListenPulseMs = 160;

  enum class Event : uint8_t { None, EnableAutoListen, DisableAutoListen };

  AssistantVoiceGesture();

  Event press(uint32_t nowMs, bool autoListenEnabled);
  Event tick(uint32_t nowMs, bool touchedInsideControl);
  void release();
  void reset();
  bool pulsing(uint32_t nowMs) const;

private:
  bool _holdPending;
  uint32_t _holdStartedMs;
  uint32_t _pulseUntilMs;
};
