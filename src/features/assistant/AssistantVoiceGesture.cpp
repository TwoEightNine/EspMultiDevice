#include "AssistantVoiceGesture.h"

AssistantVoiceGesture::AssistantVoiceGesture()
    : _holdPending(false), _holdStartedMs(0), _pulseUntilMs(0) {}

AssistantVoiceGesture::Event AssistantVoiceGesture::press(uint32_t nowMs,
                                                           bool autoListenEnabled) {
  _holdPending = false;
  _holdStartedMs = 0;
  if (autoListenEnabled) return Event::DisableAutoListen;
  _holdPending = true;
  _holdStartedMs = nowMs;
  return Event::None;
}

AssistantVoiceGesture::Event AssistantVoiceGesture::tick(uint32_t nowMs,
                                                          bool touchedInsideControl) {
  if (!_holdPending) return Event::None;
  if (!touchedInsideControl) {
    _holdPending = false;
    return Event::None;
  }
  if (nowMs - _holdStartedMs < AutoListenHoldMs) return Event::None;
  _holdPending = false;
  _pulseUntilMs = nowMs + AutoListenPulseMs;
  return Event::EnableAutoListen;
}

void AssistantVoiceGesture::release() { _holdPending = false; }

void AssistantVoiceGesture::reset() {
  _holdPending = false;
  _holdStartedMs = 0;
  _pulseUntilMs = 0;
}

bool AssistantVoiceGesture::pulsing(uint32_t nowMs) const {
  return _pulseUntilMs != 0 && static_cast<int32_t>(nowMs - _pulseUntilMs) < 0;
}
