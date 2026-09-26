#include "AssistantController.h"

#include "../audio/MicController.h"
#include "../network/WiFiController.h"
#include "../storage/SdCardController.h"

#include <string.h>

AssistantController::AssistantController(WiFiController &wifi, TimeController &time,
                                         MicController &mic, SpeakerController &speaker,
                                         SdCardController &sdCard,
                                         const AssistantBackendConfig &config)
    : _wifi(wifi), _time(time), _mic(mic), _speaker(speaker), _sdCard(sdCard), _config(config), _session(nullptr),
      _snapshotMutex(nullptr), _snapshot{}, _transcript{}, _generation(0), _begun(false),
      _screenActive(false), _deactivationPending(false), _disposeSession(false),
      _lastWifiConnected(false), _resetPending(false), _turnPending(false), _pendingAudioLaunch() {
  _snapshot.clear();
}

bool AssistantController::begin() {
  if (_begun) return true;
  _snapshotMutex = xSemaphoreCreateMutex();
  if (!_snapshotMutex) {
    setLocalError(AssistantError::Internal, false, "Assistant controller unavailable");
    return false;
  }
  _begun = true;
  _lastWifiConnected = _wifi.isConnected();
  return true;
}

void AssistantController::activate() {
  if (!_begun || _screenActive) return;
  if (!_transcript.begin()) {
    setLocalError(AssistantError::Internal, true, "Caption memory unavailable");
    return;
  }
  _transcript.clear();
  if (!ensureSession()) return;
  _screenActive = true;
  _deactivationPending = false;
  _turnPending = false;
  _lastWifiConnected = _wifi.isConnected();
  const uint32_t generation = nextGeneration();
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.generation = generation;
  _snapshot.sessionActive = true;
  _snapshot.micAvailable = _mic.isReady() && _mic.isPowered();
  _snapshot.retryable = false;
  _snapshot.error = AssistantError::None;
  _snapshot.autoListen = false;
  _snapshot.resettingConversation = false;
  _snapshot.sttCaption[0] = 0;
  _snapshot.ttsCaption[0] = 0;
  syncTranscriptMetadata();
  _snapshot.phase = _lastWifiConnected ? AssistantPhase::Bootstrapping
                                       : AssistantPhase::NeedsWiFi;
  copyBounded(_lastWifiConnected ? "Preparing secure session" : "Connect to Wi-Fi",
              _snapshot.status, sizeof(_snapshot.status));
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
  if (!send(XiaozhiSessionCommandType::Activate, generation)) {
    setLocalError(AssistantError::QueueOverflow, true, "Assistant command queue full");
  }
}

void AssistantController::deactivate() {
  if (!_begun || !_screenActive || _deactivationPending) return;
  const uint32_t generation = nextGeneration();
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.generation = generation;
  _snapshot.autoListen = false;
  _snapshot.resettingConversation = false;
  _resetPending = false;
  _turnPending = false;
  copyBounded("Closing safely", _snapshot.status, sizeof(_snapshot.status));
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
  if (send(XiaozhiSessionCommandType::Deactivate, generation)) {
    _deactivationPending = true;
  } else {
    setLocalError(AssistantError::QueueOverflow, false, "Cannot stop assistant worker");
  }
}

void AssistantController::startTurn() {
  const AssistantSnapshot current = snapshot();
  if (!_begun || !_screenActive || current.phase != AssistantPhase::Ready ||
      _turnPending) return;
  _turnPending = true;
  const uint32_t generation = nextGeneration();
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _transcript.beginDraft();
  _snapshot.generation = generation;
  _snapshot.error = AssistantError::None;
  _snapshot.retryable = false;
  _snapshot.emotion = AssistantEmotion::Neutral;
  _snapshot.sttCaption[0] = 0;
  _snapshot.ttsCaption[0] = 0;
  copyBounded("Starting microphone", _snapshot.status, sizeof(_snapshot.status));
  syncTranscriptMetadata();
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
  if (!send(XiaozhiSessionCommandType::StartTurn, generation)) {
    _turnPending = false;
    xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
    _transcript.discardDraft();
    syncTranscriptMetadata();
    xSemaphoreGive(_snapshotMutex);
    setLocalError(AssistantError::QueueOverflow, true, "Assistant command queue full");
  }
}

void AssistantController::setAutoListen(bool enabled) {
  if (!_begun || !_screenActive) return;
  bool start = false;
  bool cancel = false;
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.autoListen = enabled;
  start = enabled && _snapshot.phase == AssistantPhase::Ready && !_resetPending;
  cancel = !enabled && _snapshot.phase == AssistantPhase::Listening;
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
  if (cancel) cancelTurn();
  else if (start) startTurn();
}

void AssistantController::newConversation() {
  if (!_begun || !_screenActive || _resetPending) return;
  const AssistantSnapshot current = snapshot();
  _resetPending = true;
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.autoListen = false;
  _snapshot.resettingConversation = true;
  copyBounded("Resetting conversation", _snapshot.status, sizeof(_snapshot.status));
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
  if (turnActive(current.phase)) cancelTurn();
  else if (!sendResetConversation()) {
    _resetPending = false;
    xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
    _snapshot.resettingConversation = false;
    xSemaphoreGive(_snapshotMutex);
    setLocalError(AssistantError::QueueOverflow, true, "Reset command queue full");
  }
}

void AssistantController::cancelTurn() {
  const AssistantSnapshot current = snapshot();
  if (!_begun || !_screenActive || !turnActive(current.phase)) return;
  const uint32_t generation = nextGeneration();
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.generation = generation;
  copyBounded("Cancelling", _snapshot.status, sizeof(_snapshot.status));
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
  if (!send(XiaozhiSessionCommandType::CancelTurn, generation)) {
    setLocalError(AssistantError::QueueOverflow, true, "Assistant command queue full");
  }
}

void AssistantController::retry() {
  if (!_begun || !_screenActive) return;
  const AssistantSnapshot current = snapshot();
  if (current.phase != AssistantPhase::Error || !current.retryable) return;
  const uint32_t generation = nextGeneration();
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.generation = generation;
  _snapshot.phase = AssistantPhase::Bootstrapping;
  _snapshot.error = AssistantError::None;
  _snapshot.retryable = false;
  copyBounded("Preparing secure session", _snapshot.status, sizeof(_snapshot.status));
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
  if (!send(XiaozhiSessionCommandType::Retry, generation)) {
    setLocalError(AssistantError::QueueOverflow, true, "Assistant command queue full");
  }
}

void AssistantController::tick(uint32_t nowMs) {
  if (!_begun) return;
  const bool wifiConnected = _wifi.isConnected();
  if (_screenActive && !_disposeSession && _lastWifiConnected && !wifiConnected) {
    const uint32_t generation = nextGeneration();
    send(XiaozhiSessionCommandType::Retry, generation);
    xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
    _snapshot.generation = generation;
    _snapshot.phase = AssistantPhase::NeedsWiFi;
    _snapshot.error = AssistantError::None;
    _snapshot.retryable = false;
    _transcript.clear();
    _snapshot.sttCaption[0] = 0;
    _snapshot.ttsCaption[0] = 0;
    syncTranscriptMetadata();
    copyBounded("Connection lost; captions reset", _snapshot.status,
                sizeof(_snapshot.status));
    ++_snapshot.revision;
    xSemaphoreGive(_snapshotMutex);
  } else if (_screenActive && !_disposeSession && !_lastWifiConnected && wifiConnected) {
    const uint32_t generation = nextGeneration();
    xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
    _snapshot.generation = generation;
    _snapshot.phase = AssistantPhase::Bootstrapping;
    copyBounded("Preparing secure session", _snapshot.status, sizeof(_snapshot.status));
    ++_snapshot.revision;
    xSemaphoreGive(_snapshotMutex);
    send(XiaozhiSessionCommandType::Retry, generation);
  }
  _lastWifiConnected = wifiConnected;

  XiaozhiSessionEvent event = {};
  uint8_t drained = 0;
  while (_session && drained++ < XiaozhiSession::EventQueueCapacity && _session->poll(event)) {
    if (event.generation == _generation) apply(event);
    memset(&event, 0, sizeof(event));
  }
  if (_disposeSession && _session && !_session->running()) {
    // The worker sends Deactivated only after full session teardown and
    // credential wiping.
    // Release its large WebSocket and codec members off-screen so browser TLS
    // keeps its internal-heap contiguous block.
    (void)nowMs;
    disposeSession();
  }
}

AssistantSnapshot AssistantController::snapshot() const {
  AssistantSnapshot result = {};
  if (!_snapshotMutex) {
    result = _snapshot;
    return result;
  }
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  result = _snapshot;
  xSemaphoreGive(_snapshotMutex);
  return result;
}

bool AssistantController::copyTranscriptTurn(
    uint8_t index, AssistantTranscriptTurn &output) const {
  if (!_snapshotMutex) return false;
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  const bool copied = _transcript.copyTurn(index, output);
  xSemaphoreGive(_snapshotMutex);
  return copied;
}

bool AssistantController::copyTranscriptDraft(AssistantTranscriptTurn &output) const {
  if (!_snapshotMutex) return false;
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  const bool copied = _transcript.copyDraft(output);
  xSemaphoreGive(_snapshotMutex);
  return copied;
}

bool AssistantController::takeAudioLaunch(String &path) {
  path = "";
  if (!_snapshotMutex) return false;
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  if (_pendingAudioLaunch.length()) {
    path = _pendingAudioLaunch;
    _pendingAudioLaunch = "";
  }
  xSemaphoreGive(_snapshotMutex);
  return path.length() > 0;
}

uint32_t AssistantController::nextGeneration() {
  ++_generation;
  if (_generation == 0) ++_generation;
  return _generation;
}

bool AssistantController::send(XiaozhiSessionCommandType type, uint32_t generation) {
  return _session && _session->post({type, generation});
}

bool AssistantController::sendResetConversation() {
  const uint32_t generation = nextGeneration();
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.generation = generation;
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
  return send(XiaozhiSessionCommandType::ResetConversation, generation);
}

void AssistantController::syncTranscriptMetadata() {
  _snapshot.transcriptRevision = _transcript.revision();
  _snapshot.transcriptCount = _transcript.count();
}

void AssistantController::apply(const XiaozhiSessionEvent &event) {
  bool startNext = false;
  bool sendReset = false;
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  const AssistantPhase previousPhase = _snapshot.phase;
  if (event.type == XiaozhiSessionEventType::Phase) {
    if (event.phase == AssistantPhase::Listening ||
        event.phase == AssistantPhase::Ready) _turnPending = false;
    _snapshot.phase = event.phase;
    _snapshot.micAvailable = event.micAvailable;
    _snapshot.error = AssistantError::None;
    _snapshot.retryable = false;
    copyBounded(event.text, _snapshot.status, sizeof(_snapshot.status));
    if (event.phase == AssistantPhase::Ready) {
      if (previousPhase == AssistantPhase::Speaking) _transcript.completeDraft();
      else if (strcmp(event.text, "Cancelled") == 0) _transcript.discardDraft();
      if (_resetPending) sendReset = true;
      else startNext = _snapshot.autoListen;
    }
  } else if (event.type == XiaozhiSessionEventType::Activation) {
    _snapshot.phase = AssistantPhase::NeedsActivation;
    _snapshot.micAvailable = event.micAvailable;
    copyBounded(event.code, _snapshot.activationCode, sizeof(_snapshot.activationCode));
    copyBounded(event.text, _snapshot.activationMessage, sizeof(_snapshot.activationMessage));
    copyBounded("Activation required", _snapshot.status, sizeof(_snapshot.status));
  } else if (event.type == XiaozhiSessionEventType::SttCaption) {
    copyBounded(event.text, _snapshot.sttCaption, sizeof(_snapshot.sttCaption));
    _transcript.setUser(event.text);
  } else if (event.type == XiaozhiSessionEventType::TtsCaption) {
    copyBounded(event.text, _snapshot.ttsCaption, sizeof(_snapshot.ttsCaption));
    _transcript.appendAssistant(event.text);
  } else if (event.type == XiaozhiSessionEventType::Emotion) {
    _snapshot.emotion = event.emotion;
  } else if (event.type == XiaozhiSessionEventType::Error) {
    _turnPending = false;
    _transcript.discardDraft();
    if (_resetPending) {
      _resetPending = false;
      _snapshot.resettingConversation = false;
    }
    _snapshot.phase = AssistantPhase::Error;
    _snapshot.micAvailable = event.micAvailable;
    _snapshot.error = event.error;
    _snapshot.retryable = event.retryable;
    copyBounded(assistantErrorName(event.error), _snapshot.status, sizeof(_snapshot.status));
  } else if (event.type == XiaozhiSessionEventType::ConversationReset) {
    _transcript.clear();
    _resetPending = false;
    _snapshot.resettingConversation = false;
    _snapshot.sttCaption[0] = 0;
    _snapshot.ttsCaption[0] = 0;
    copyBounded(event.text, _snapshot.status, sizeof(_snapshot.status));
  } else if (event.type == XiaozhiSessionEventType::ContextLost) {
    _turnPending = false;
    _transcript.clear();
    _snapshot.sttCaption[0] = 0;
    _snapshot.ttsCaption[0] = 0;
    copyBounded(event.text, _snapshot.status, sizeof(_snapshot.status));
  } else if (event.type == XiaozhiSessionEventType::LaunchAudio) {
    if (event.audioPath[0]) {
      _pendingAudioLaunch = String(event.audioPath);
      _snapshot.autoListen = false;
      copyBounded("Opening audio player", _snapshot.status, sizeof(_snapshot.status));
    }
  } else if (event.type == XiaozhiSessionEventType::Deactivated) {
    _disposeSession = true;
    copyBounded("Closing safely", _snapshot.status, sizeof(_snapshot.status));
  }
  syncTranscriptMetadata();
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
  if (sendReset && !sendResetConversation()) {
    _resetPending = false;
    setLocalError(AssistantError::QueueOverflow, true, "Reset command queue full");
  } else if (startNext) {
    startTurn();
  }
}

bool AssistantController::ensureSession() {
  if (_session) return true;
  _session = new XiaozhiSession(_wifi, _time, _mic, _speaker, _sdCard, _config);
  if (_session && _session->begin()) return true;
  disposeSession();
  _screenActive = false;
  _deactivationPending = false;
  _turnPending = false;
  _resetPending = false;
  _pendingAudioLaunch = "";
  setLocalError(AssistantError::Internal, true, "Assistant worker unavailable");
  return false;
}

void AssistantController::disposeSession() {
  _disposeSession = false;
  if (_session) {
    delete _session;
    _session = nullptr;
  }
  _screenActive = false;
  _deactivationPending = false;
  _turnPending = false;
  _resetPending = false;
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _transcript.end();
  _snapshot.sessionActive = false;
  _snapshot.autoListen = false;
  _snapshot.resettingConversation = false;
  _snapshot.sttCaption[0] = 0;
  _snapshot.ttsCaption[0] = 0;
  syncTranscriptMetadata();
  _snapshot.phase = AssistantPhase::Ready;
  copyBounded("Closed", _snapshot.status, sizeof(_snapshot.status));
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
}

void AssistantController::setLocalError(AssistantError error, bool retryable,
                                        const char *status) {
  if (!_snapshotMutex) {
    _snapshot.phase = AssistantPhase::Error;
    _snapshot.error = error;
    _snapshot.retryable = retryable;
    copyBounded(status, _snapshot.status, sizeof(_snapshot.status));
    return;
  }
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.phase = AssistantPhase::Error;
  _snapshot.error = error;
  _snapshot.retryable = retryable;
  copyBounded(status, _snapshot.status, sizeof(_snapshot.status));
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
}

void AssistantController::mutatePhase(AssistantPhase phase, const char *status) {
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.phase = phase;
  copyBounded(status, _snapshot.status, sizeof(_snapshot.status));
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
}

bool AssistantController::turnActive(AssistantPhase phase) {
  return phase == AssistantPhase::Connecting || phase == AssistantPhase::Listening ||
         phase == AssistantPhase::Thinking || phase == AssistantPhase::Speaking ||
         phase == AssistantPhase::Bootstrapping ||
         phase == AssistantPhase::NeedsActivation;
}

void AssistantController::copyBounded(const char *value, char *output, size_t capacity) {
  XiaozhiProtocol::copyUtf8Bounded(value, output, capacity);
}
