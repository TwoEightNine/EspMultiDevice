#include "AssistantController.h"

#include "../audio/MicController.h"
#include "../network/WiFiController.h"

#include <string.h>

AssistantController::AssistantController(WiFiController &wifi, TimeController &time,
                                         MicController &mic, SpeakerController &speaker,
                                         const AssistantBackendConfig &config)
    : _wifi(wifi), _time(time), _mic(mic), _speaker(speaker), _config(config), _session(nullptr),
      _snapshotMutex(nullptr), _snapshot{}, _generation(0), _begun(false),
      _screenActive(false), _deactivationPending(false), _disposeSession(false),
      _lastWifiConnected(false) {
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
  if (!ensureSession()) return;
  _screenActive = true;
  _deactivationPending = false;
  _lastWifiConnected = _wifi.isConnected();
  const uint32_t generation = nextGeneration();
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.generation = generation;
  _snapshot.sessionActive = true;
  _snapshot.retryable = false;
  _snapshot.error = AssistantError::None;
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
  if (!_begun || !_screenActive || current.phase != AssistantPhase::Ready) return;
  const uint32_t generation = nextGeneration();
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.generation = generation;
  _snapshot.phase = AssistantPhase::Connecting;
  _snapshot.error = AssistantError::None;
  _snapshot.retryable = false;
  _snapshot.emotion = AssistantEmotion::Neutral;
  _snapshot.sttCaption[0] = 0;
  _snapshot.ttsCaption[0] = 0;
  copyBounded("Connecting", _snapshot.status, sizeof(_snapshot.status));
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
  if (!send(XiaozhiSessionCommandType::StartTurn, generation)) {
    setLocalError(AssistantError::QueueOverflow, true, "Assistant command queue full");
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
    copyBounded("Connect to Wi-Fi", _snapshot.status, sizeof(_snapshot.status));
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
    // The worker sends Deactivated only after safeStop() and credential wiping.
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

uint32_t AssistantController::nextGeneration() {
  ++_generation;
  if (_generation == 0) ++_generation;
  return _generation;
}

bool AssistantController::send(XiaozhiSessionCommandType type, uint32_t generation) {
  return _session && _session->post({type, generation});
}

void AssistantController::apply(const XiaozhiSessionEvent &event) {
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  if (event.type == XiaozhiSessionEventType::Phase) {
    _snapshot.phase = event.phase;
    _snapshot.micAvailable = event.micAvailable;
    _snapshot.error = AssistantError::None;
    _snapshot.retryable = false;
    copyBounded(event.text, _snapshot.status, sizeof(_snapshot.status));
  } else if (event.type == XiaozhiSessionEventType::Activation) {
    _snapshot.phase = AssistantPhase::NeedsActivation;
    _snapshot.micAvailable = event.micAvailable;
    copyBounded(event.code, _snapshot.activationCode, sizeof(_snapshot.activationCode));
    copyBounded(event.text, _snapshot.activationMessage, sizeof(_snapshot.activationMessage));
    copyBounded("Activation required", _snapshot.status, sizeof(_snapshot.status));
  } else if (event.type == XiaozhiSessionEventType::SttCaption) {
    copyBounded(event.text, _snapshot.sttCaption, sizeof(_snapshot.sttCaption));
  } else if (event.type == XiaozhiSessionEventType::TtsCaption) {
    copyBounded(event.text, _snapshot.ttsCaption, sizeof(_snapshot.ttsCaption));
  } else if (event.type == XiaozhiSessionEventType::Emotion) {
    _snapshot.emotion = event.emotion;
  } else if (event.type == XiaozhiSessionEventType::Error) {
    _snapshot.phase = AssistantPhase::Error;
    _snapshot.micAvailable = event.micAvailable;
    _snapshot.error = event.error;
    _snapshot.retryable = event.retryable;
    copyBounded(assistantErrorName(event.error), _snapshot.status, sizeof(_snapshot.status));
  } else if (event.type == XiaozhiSessionEventType::Deactivated) {
    _disposeSession = true;
    copyBounded("Closing safely", _snapshot.status, sizeof(_snapshot.status));
  }
  ++_snapshot.revision;
  xSemaphoreGive(_snapshotMutex);
}

bool AssistantController::ensureSession() {
  if (_session) return true;
  _session = new XiaozhiSession(_wifi, _time, _mic, _speaker, _config);
  if (_session && _session->begin()) return true;
  disposeSession();
  _screenActive = false;
  _deactivationPending = false;
  setLocalError(AssistantError::Internal, true, "Assistant worker unavailable");
  return false;
}

void AssistantController::disposeSession() {
  _disposeSession = false;
  if (!_session) return;
  delete _session;
  _session = nullptr;
  _screenActive = false;
  _deactivationPending = false;
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  _snapshot.sessionActive = false;
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
