#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "AssistantState.h"
#include "AssistantTranscript.h"
#include "XiaozhiBootstrap.h"
#include "XiaozhiSession.h"

class MicController;
class SpeakerController;
class TimeController;
class WiFiController;
class SdCardController;

class AssistantController {
public:
  AssistantController(WiFiController &wifi, TimeController &time, MicController &mic,
                      SpeakerController &speaker, SdCardController &sdCard,
                      const AssistantBackendConfig &config);

  bool begin();
  void activate();
  void deactivate();
  void startTurn();
  void cancelTurn();
  void setAutoListen(bool enabled);
  void newConversation();
  void retry();
  void tick(uint32_t nowMs);
  AssistantSnapshot snapshot() const;
  bool copyTranscriptTurn(uint8_t index, AssistantTranscriptTurn &output) const;
  bool copyTranscriptDraft(AssistantTranscriptTurn &output) const;
  // Returns one accepted assistant-owned playback launch. The UI consumes it
  // only after the worker has been safely deactivated.
  bool takeAudioLaunch(String &path);

private:
  WiFiController &_wifi;
  TimeController &_time;
  MicController &_mic;
  SpeakerController &_speaker;
  SdCardController &_sdCard;
  AssistantBackendConfig _config;
  XiaozhiSession *_session;
  mutable SemaphoreHandle_t _snapshotMutex;
  AssistantSnapshot _snapshot;
  AssistantTranscript _transcript;
  uint32_t _generation;
  bool _begun;
  bool _screenActive;
  bool _deactivationPending;
  bool _disposeSession;
  bool _lastWifiConnected;
  bool _resetPending;
  bool _turnPending;
  String _pendingAudioLaunch;

  bool ensureSession();
  void disposeSession();
  uint32_t nextGeneration();
  bool send(XiaozhiSessionCommandType type, uint32_t generation);
  bool sendResetConversation();
  void syncTranscriptMetadata();
  void apply(const XiaozhiSessionEvent &event);
  void setLocalError(AssistantError error, bool retryable, const char *status);
  void mutatePhase(AssistantPhase phase, const char *status);
  static bool turnActive(AssistantPhase phase);
  static void copyBounded(const char *value, char *output, size_t capacity);
};
