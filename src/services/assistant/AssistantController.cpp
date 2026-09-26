#include "AssistantController.h"

#include "../audio/MicController.h"
#include "../audio/AudioSettings.h"
#include "../network/WiFiController.h"
#include "../network/TimeController.h"
#include "../storage/SdCardController.h"
#include "../storage/BatteryHistoryService.h"
#include "../led/LedSettings.h"

#include <math.h>

#include <string.h>

AssistantController::AssistantController(WiFiController &wifi, TimeController &time,
                                         MicController &mic, SpeakerController &speaker,
                                         SdCardController &sdCard,
                                         BatteryHistoryService &batteryHistory,
                                         AudioSettings &audioSettings, LedSettings &ledSettings,
                                         const AssistantBackendConfig &config)
    : _wifi(wifi), _time(time), _mic(mic), _speaker(speaker), _sdCard(sdCard),
      _batteryHistory(batteryHistory), _audioSettings(audioSettings), _ledSettings(ledSettings),
      _config(config), _session(nullptr),
      _snapshotMutex(nullptr), _snapshot{}, _transcript{}, _generation(0), _begun(false),
      _screenActive(false), _deactivationPending(false), _disposeSession(false),
      _lastWifiConnected(false), _resetPending(false), _turnPending(false), _pendingAudioLaunch(),
      _pendingLaunchKind(XiaozhiProtocol::DeviceCommandKind::None), _pendingLaunchValue() {
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
  _pendingAudioLaunch = "";
  _pendingLaunchKind = XiaozhiProtocol::DeviceCommandKind::None;
  _pendingLaunchValue = "";
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
    if (event.generation == _generation) {
      if (event.type == XiaozhiSessionEventType::UiAction) handleUiAction(event);
      else apply(event);
    }
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

bool AssistantController::takeDeviceLaunch(XiaozhiProtocol::DeviceCommandKind &kind,
                                          String &value) {
  kind = XiaozhiProtocol::DeviceCommandKind::None;
  value = "";
  if (!_snapshotMutex) return false;
  xSemaphoreTake(_snapshotMutex, portMAX_DELAY);
  kind = _pendingLaunchKind;
  value = _pendingLaunchValue;
  _pendingLaunchKind = XiaozhiProtocol::DeviceCommandKind::None;
  _pendingLaunchValue = "";
  xSemaphoreGive(_snapshotMutex);
  return kind != XiaozhiProtocol::DeviceCommandKind::None;
}

void AssistantController::handleUiAction(const XiaozhiSessionEvent &event) {
  if (!_session) return;
  using Kind = XiaozhiProtocol::DeviceCommandKind;
  XiaozhiProtocol::DeviceToolResult result = {};
  result.clear();
  result.command = event.deviceCommand;
  result.ok = true;
  switch (event.deviceCommand) {
    case Kind::GetTime: {
      struct tm local = {};
      result.available = _time.localTime(local);
      if (result.available)
        snprintf(result.localTime, sizeof(result.localTime),
                 "%04d-%02d-%02dT%02d:%02d:%02d+04:00",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 local.tm_hour, local.tm_min, local.tm_sec);
      break;
    }
    case Kind::GetWifiStatus: {
      static constexpr const char *States[] = {
        "disabled", "disconnected", "scanning", "connecting", "connected", "failed"
      };
      const uint8_t state = static_cast<uint8_t>(_wifi.state());
      strcpy(result.state, state < 6 ? States[state] : "failed");
      break;
    }
    case Kind::GetWifiSignal:
      result.connected = _wifi.isConnected();
      if (result.connected) result.rssiDbm = constrain(WiFi.RSSI(), -127, 0);
      break;
    case Kind::GetSdStatus:
      result.present = _sdCard.isCardPresent();
      result.mounted = _sdCard.isMounted();
      strcpy(result.state, _sdCard.owner() == SdCardOwner::Device ? "device" :
             _sdCard.owner() == SdCardOwner::UsbHost ? "usb_host" : "none");
      break;
    case Kind::GetMicStatus:
      result.ready = _mic.isReady();
      result.powered = _mic.isPowered();
      result.powerSensePresent = _mic.hasPowerSense();
      break;
    case Kind::GetBatteryStatus: {
      const BatteryReading reading = _batteryHistory.latest().raw;
      if (isfinite(reading.voltage) && reading.voltage > 0.0f && reading.voltage <= 6.0f) {
        result.voltageMv = static_cast<int32_t>(lroundf(reading.voltage * 1000.0f));
        result.percentage = reading.percentage;
      } else {
        result.ok = false;
        result.error = XiaozhiProtocol::DeviceToolError::Unavailable;
      }
      break;
    }
    case Kind::GetVolume:
      result.level = _audioSettings.volume();
      break;
    case Kind::GetLedStatus:
      result.available = _ledSettings.isAvailable();
      result.enabled = _ledSettings.isEnabled();
      strncpy(result.color, LedSettings::colorName(_ledSettings.color()), sizeof(result.color) - 1);
      break;
    case Kind::SetVolume:
      result.ok = _audioSettings.setVolume(event.level);
      result.level = _audioSettings.volume();
      if (!result.ok) result.error = XiaozhiProtocol::DeviceToolError::SettingsFailed;
      break;
    case Kind::SetLedEnabled:
      result.ok = _ledSettings.setEnabled(event.enabled);
      result.available = _ledSettings.isAvailable();
      result.enabled = _ledSettings.isEnabled();
      strncpy(result.color, LedSettings::colorName(_ledSettings.color()), sizeof(result.color) - 1);
      if (!result.ok) result.error = result.available
          ? XiaozhiProtocol::DeviceToolError::SettingsFailed
          : XiaozhiProtocol::DeviceToolError::Unavailable;
      break;
    case Kind::SetLedColor: {
      static constexpr const char *Colors[] = {"red", "green", "blue", "yellow", "cyan", "magenta"};
      uint8_t index = 0;
      while (index < 6 && strcmp(event.text, Colors[index])) ++index;
      result.ok = index < 6 && _ledSettings.setColor(static_cast<LedColor>(index));
      result.available = _ledSettings.isAvailable();
      result.enabled = _ledSettings.isEnabled();
      strncpy(result.color, LedSettings::colorName(_ledSettings.color()), sizeof(result.color) - 1);
      if (!result.ok) result.error = result.available
          ? XiaozhiProtocol::DeviceToolError::SettingsFailed
          : XiaozhiProtocol::DeviceToolError::Unavailable;
      break;
    }
    default:
      result.ok = false;
      result.error = XiaozhiProtocol::DeviceToolError::Protocol;
      break;
  }
  _session->completeUiAction(event.requestId, result);
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
  } else if (event.type == XiaozhiSessionEventType::LaunchDevice) {
    _pendingLaunchKind = event.deviceCommand;
    _pendingLaunchValue = event.launchValue;
    _snapshot.autoListen = false;
    copyBounded("Opening", _snapshot.status, sizeof(_snapshot.status));
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
