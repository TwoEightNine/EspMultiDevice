#include "XiaozhiSession.h"

#include "../audio/MicController.h"
#include "../audio/SpeakerController.h"
#include "../network/TimeController.h"
#include "../network/WiFiController.h"
#include "../storage/SdCardController.h"

#include <esp_heap_caps.h>
#include <string.h>

namespace {
constexpr uint32_t SecureTimeTimeoutMs = 30000;
constexpr uint32_t ActivationPollMs = 3000;
constexpr uint32_t ActivationMinMs = 30000;
constexpr uint32_t ActivationMaxMs = 10U * 60U * 1000U;
constexpr uint32_t HelloTimeoutMs = 10000;
constexpr uint32_t ListeningTimeoutMs = 45000;
constexpr uint32_t ThinkingTimeoutMs = 60000;
constexpr uint32_t SpeakingPacketTimeoutMs = 15000;
constexpr uint32_t SpeakingTotalTimeoutMs = 120000;
constexpr uint32_t TurnTimeoutMs = 180000;
constexpr uint32_t HeartbeatIntervalMs = 60000;
constexpr uint32_t HeartbeatTimeoutMs = 10000;
constexpr uint32_t ConversationResetTimeoutMs = 10000;
constexpr uint32_t RetryDelaysMs[] = {2000, 4000, 8000, 15000};
constexpr size_t AudioEnvelopeBytes = XiaozhiProtocol::MaxAudioPacketBytes + 16;

void copyText(const char *input, char *output, size_t capacity) {
  if (!output || !capacity) return;
  if (!input) input = "";
  const size_t length = strnlen(input, capacity - 1);
  memcpy(output, input, length);
  output[length] = 0;
}

const char *deviceToolErrorName(XiaozhiProtocol::DeviceToolError error) {
  switch (error) {
    case XiaozhiProtocol::DeviceToolError::None: return "none";
    case XiaozhiProtocol::DeviceToolError::InvalidArguments: return "invalid_arguments";
    case XiaozhiProtocol::DeviceToolError::StorageUnavailable: return "storage_unavailable";
    case XiaozhiProtocol::DeviceToolError::Busy: return "busy";
    case XiaozhiProtocol::DeviceToolError::SelectionUnavailable: return "selection_unavailable";
    case XiaozhiProtocol::DeviceToolError::Protocol: return "protocol";
  }
  return "unknown";
}
}  // namespace

XiaozhiSession::XiaozhiSession(WiFiController &wifi, TimeController &time,
                               MicController &mic, SpeakerController &speaker,
                               SdCardController &sdCard,
                               const AssistantBackendConfig &config)
    : _wifi(wifi), _time(time), _mic(mic), _speaker(speaker), _sdCard(sdCard), _identity(),
      _bootstrap(config), _config(config), _websocket(), _encoder(), _decoder(),
      _bootstrapResponse(nullptr), _commands(nullptr), _events(nullptr),
      _commandStorage(nullptr), _eventStorage(nullptr), _commandQueueControl{},
      _eventQueueControl{}, _task(nullptr),
      _requestedGeneration(0), _cancelRequested(false), _deactivateRequested(false),
      _workerExited(false),
      _screenActive(false), _fatalError(false), _credentialsRejected(false),
      _generation(0), _retryIndex(0), _lastHeartbeatMs(0), _heartbeatSentMs(0),
      _heartbeatPending(false), _sessionId{},
      _uplink(nullptr), _downlink(nullptr), _decodedPcm(nullptr), _uplinkHead(0), _uplinkCount(0),
      _downlinkHead(0), _downlinkCount(0), _receivedDownlinkPackets(0),
      _receivedDownlinkBytes(0), _playedPcmSamples(0), _peakPcm(0),
      _deviceTools(_sdCard), _lastDeviceCommandId(0) {
}

XiaozhiSession::~XiaozhiSession() {
  _deactivateRequested.store(true, std::memory_order_release);
  if (_task && !_workerExited.load(std::memory_order_acquire)) {
    vTaskDelete(_task);
  }
  _task = nullptr;
  stopSession(false);
  clearCredentials();
  if (_commands) vQueueDelete(_commands);
  if (_events) vQueueDelete(_events);
  if (_commandStorage) {
    memset(_commandStorage, 0, sizeof(XiaozhiSessionCommand) * CommandQueueCapacity);
    free(_commandStorage);
  }
  if (_eventStorage) {
    memset(_eventStorage, 0, sizeof(XiaozhiSessionEvent) * EventQueueCapacity);
    free(_eventStorage);
  }
  if (_bootstrapResponse) {
    _bootstrapResponse->clear();
    memset(_bootstrapResponse, 0, sizeof(*_bootstrapResponse));
    free(_bootstrapResponse);
  }
  if (_uplink) { memset(_uplink, 0, sizeof(PcmFrame) * UplinkQueueCapacity); free(_uplink); }
  if (_downlink) { memset(_downlink, 0, sizeof(OpusPacket) * DownlinkQueueCapacity); free(_downlink); }
  if (_decodedPcm) {
    memset(_decodedPcm, 0, sizeof(int16_t) * AssistantAudio::OpusDecoder::MaxFrameSamples);
    free(_decodedPcm);
  }
}

bool XiaozhiSession::begin() {
  if (_task) return true;
  _commandStorage = static_cast<uint8_t *>(heap_caps_calloc(
      CommandQueueCapacity, sizeof(XiaozhiSessionCommand), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  _eventStorage = static_cast<uint8_t *>(heap_caps_calloc(
      EventQueueCapacity, sizeof(XiaozhiSessionEvent), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  _bootstrapResponse = static_cast<XiaozhiBootstrapResponse *>(heap_caps_calloc(
      1, sizeof(XiaozhiBootstrapResponse), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  _commands = _commandStorage ? xQueueCreateStatic(CommandQueueCapacity,
      sizeof(XiaozhiSessionCommand), _commandStorage, &_commandQueueControl) : nullptr;
  _events = _eventStorage ? xQueueCreateStatic(EventQueueCapacity,
      sizeof(XiaozhiSessionEvent), _eventStorage, &_eventQueueControl) : nullptr;
  _uplink = static_cast<PcmFrame *>(heap_caps_calloc(
      UplinkQueueCapacity, sizeof(PcmFrame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  _downlink = static_cast<OpusPacket *>(heap_caps_calloc(
      DownlinkQueueCapacity, sizeof(OpusPacket), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  _decodedPcm = static_cast<int16_t *>(heap_caps_calloc(
      AssistantAudio::OpusDecoder::MaxFrameSamples, sizeof(int16_t),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!_commands || !_events || !_bootstrapResponse || !_uplink || !_downlink || !_decodedPcm) return false;
  const BaseType_t core = xPortGetCoreID() == 0 ? 1 : 0;
  // Network code can disable the flash cache. Its caller's stack must remain
  // in internal RAM while that happens, so do not place this task stack in
  // PSRAM even though the bulk queue storage is external.
  return xTaskCreatePinnedToCore(workerEntry, "xiaozhi-session", WorkerStackBytes,
                                 this, tskIDLE_PRIORITY + 1, &_task, core) == pdPASS;
}

bool XiaozhiSession::post(const XiaozhiSessionCommand &command) {
  if (!_commands) return false;
  _requestedGeneration.store(command.generation, std::memory_order_release);
  if (command.type == XiaozhiSessionCommandType::CancelTurn)
    _cancelRequested.store(true, std::memory_order_release);
  if (command.type == XiaozhiSessionCommandType::Deactivate)
    _deactivateRequested.store(true, std::memory_order_release);
  return xQueueSend(_commands, &command, 0) == pdTRUE;
}

bool XiaozhiSession::poll(XiaozhiSessionEvent &event) {
  return _events && xQueueReceive(_events, &event, 0) == pdTRUE;
}

bool XiaozhiSession::running() const {
  return _task != nullptr && !_workerExited.load(std::memory_order_acquire);
}

void XiaozhiSession::workerEntry(void *context) {
  static_cast<XiaozhiSession *>(context)->worker();
}

void XiaozhiSession::worker() {
  XiaozhiSessionCommand command = {};
  for (;;) {
    if (xQueueReceive(_commands, &command, pdMS_TO_TICKS(10)) == pdTRUE) {
      const bool shouldExit = command.type == XiaozhiSessionCommandType::Deactivate &&
          command.generation == _requestedGeneration.load(std::memory_order_acquire);
      handle(command);
      if (shouldExit) {
        // PublishDeactivated has completed, so the controller can observe the
        // acknowledgement. Do not touch session storage after this point.
        _workerExited.store(true, std::memory_order_release);
        vTaskDelete(nullptr);
      }
      continue;
    }
    if (_screenActive && _websocket.isConnected() &&
        !serviceIdleConnection(_generation) && !interrupted(_generation)) {
      XiaozhiSessionEvent lost = {};
      lost.type = XiaozhiSessionEventType::ContextLost;
      lost.generation = _generation;
      lost.micAvailable = _mic.isReady() && _mic.isPowered();
      copyText("Connection lost; captions reset", lost.text, sizeof(lost.text));
      publish(lost);
      stopSession(false);
      clearCredentials();
      while (_screenActive && !interrupted(_generation)) {
        if (bootstrapUntilReady(_generation)) {
          publishPhase(_generation, AssistantPhase::Ready,
                       "Connection restored; captions reset");
          break;
        }
        if (_fatalError || interrupted(_generation)) break;
        const uint32_t wait = RetryDelaysMs[min<size_t>(_retryIndex, 3)];
        if (_retryIndex < 3) ++_retryIndex;
        if (!delayInterruptible(wait, _generation)) break;
      }
    }
  }
}

void XiaozhiSession::handle(const XiaozhiSessionCommand &command) {
  if (command.generation != _requestedGeneration.load(std::memory_order_acquire)) return;
  _generation = command.generation;
  if (command.type == XiaozhiSessionCommandType::Deactivate) {
    _deactivateRequested.store(true, std::memory_order_release);
    _screenActive = false;
    sendAbort();
    stopSession(false);
    clearCredentials();
    publishDeactivated(command.generation);
    return;
  }
  if (command.type == XiaozhiSessionCommandType::CancelTurn) {
    _cancelRequested.store(true, std::memory_order_release);
    sendAbort();
    stopTurn(false);
    _cancelRequested.store(false, std::memory_order_release);
    if (_screenActive) publishPhase(command.generation, AssistantPhase::Ready, "Cancelled");
    return;
  }
  if (command.type == XiaozhiSessionCommandType::Activate) {
    _screenActive = true;
    _cancelRequested.store(false, std::memory_order_release);
    _deactivateRequested.store(false, std::memory_order_release);
    _fatalError = false;
  }
  if (command.type == XiaozhiSessionCommandType::Retry) {
    _cancelRequested.store(false, std::memory_order_release);
    _deactivateRequested.store(false, std::memory_order_release);
    _fatalError = false;
  }

  if (command.type == XiaozhiSessionCommandType::Activate ||
      command.type == XiaozhiSessionCommandType::Retry) {
    if (!_bootstrap.validConfiguration()) {
      publishError(command.generation, AssistantError::Configuration, false);
      return;
    }
    while (_screenActive && !interrupted(command.generation)) {
      if (bootstrapUntilReady(command.generation)) break;
      if (interrupted(command.generation)) break;
      if (_fatalError) break;
      const uint32_t wait = RetryDelaysMs[min<size_t>(_retryIndex, 3)];
      if (_retryIndex < 3) ++_retryIndex;
      if (!delayInterruptible(wait, command.generation)) break;
    }
    return;
  }

  if (command.type == XiaozhiSessionCommandType::ResetConversation) {
    _cancelRequested.store(false, std::memory_order_release);
    if (!_websocket.isConnected() && !bootstrapUntilReady(command.generation)) return;
    if (resetConversation(command.generation)) {
      XiaozhiSessionEvent event = {};
      event.type = XiaozhiSessionEventType::ConversationReset;
      event.generation = command.generation;
      event.micAvailable = _mic.isReady() && _mic.isPowered();
      copyText("New conversation", event.text, sizeof(event.text));
      publish(event);
      publishPhase(command.generation, AssistantPhase::Ready, "Ready");
    }
    return;
  }

  if (command.type != XiaozhiSessionCommandType::StartTurn || !_screenActive) return;
  _cancelRequested.store(false, std::memory_order_release);
  if (!_websocket.isConnected() && !bootstrapUntilReady(command.generation)) return;
  if (runTurn(command.generation)) {
    _retryIndex = 0;
    publishPhase(command.generation, AssistantPhase::Ready, "Ready");
    return;
  }
  if (interrupted(command.generation)) {
    stopTurn(false);
    const uint32_t requested = _requestedGeneration.load(std::memory_order_acquire);
    if (_deactivateRequested.load(std::memory_order_acquire))
      publishDeactivated(requested);
    else
      publishPhase(requested, AssistantPhase::Ready, "Cancelled");
    return;
  }
  if (_credentialsRejected ||
      _websocket.lastFailure() == SecureWebSocketClient::FailureKind::AuthenticationRejected) {
    XiaozhiSessionEvent lost = {};
    lost.type = XiaozhiSessionEventType::ContextLost;
    lost.generation = command.generation;
    lost.micAvailable = _mic.isReady() && _mic.isPowered();
    copyText("Connection lost; captions reset", lost.text, sizeof(lost.text));
    publish(lost);
    stopSession(false);
    clearCredentials();
    bootstrapUntilReady(command.generation);
    return;
  }
  XiaozhiSessionEvent lost = {};
  lost.type = XiaozhiSessionEventType::ContextLost;
  lost.generation = command.generation;
  lost.micAvailable = _mic.isReady() && _mic.isPowered();
  copyText("Connection lost; captions reset", lost.text, sizeof(lost.text));
  publish(lost);
  const uint32_t wait = RetryDelaysMs[min<size_t>(_retryIndex, 3)];
  if (_retryIndex < 3) ++_retryIndex;
  if (delayInterruptible(wait, command.generation)) bootstrapUntilReady(command.generation);
}

bool XiaozhiSession::bootstrapUntilReady(uint32_t generation) {
  stopSession(false);
  clearCredentials();
  if (!_wifi.isConnected()) {
    publishPhase(generation, AssistantPhase::NeedsWiFi, "Connect to Wi-Fi");
    while (_screenActive && !_wifi.isConnected()) {
      if (interrupted(generation)) return false;
      vTaskDelay(pdMS_TO_TICKS(100));
    }
  }
  if (!_screenActive || interrupted(generation)) return false;
  publishPhase(generation, AssistantPhase::Bootstrapping, "Preparing secure session");
  if (!waitForSecureTime(generation)) {
    if (!interrupted(generation)) publishError(generation, AssistantError::SecureTime, true);
    return false;
  }
  if (!_identity.begin()) {
    publishError(generation, AssistantError::Identity, false);
    return false;
  }
  const uint32_t started = millis();
  if (!_bootstrap.check(_identity, *_bootstrapResponse)) {
    const int httpStatus = _bootstrap.lastHttpStatus();
    const int transportError = _bootstrap.lastTransportError();
    const char *failure = httpStatus <= 0 ? "transport" :
                          httpStatus == 200 ? "response" : "http";
    Serial.print(F("Assistant bootstrap failure generation="));
    Serial.print(generation);
    Serial.print(F(" class="));
    Serial.print(failure);
    Serial.print(F(" http_status="));
    Serial.print(httpStatus);
    if (httpStatus <= 0) {
      Serial.print(F(" transport_error="));
      Serial.print(transportError);
    }
    Serial.println();
    publishError(generation,
                 millis() - started >= 15000 ? AssistantError::TimeoutBootstrap
                 : httpStatus <= 0 ? AssistantError::Network
                                   : AssistantError::Bootstrap,
                 true);
    return false;
  }
  if (interrupted(generation)) { clearCredentials(); return false; }
  if (_bootstrapResponse->activation.present) {
    XiaozhiSessionEvent event = {};
    event.type = XiaozhiSessionEventType::Activation;
    event.generation = generation;
    event.phase = AssistantPhase::NeedsActivation;
    event.micAvailable = _mic.isReady() && _mic.isPowered();
    copyText(_bootstrapResponse->activation.message, event.text, sizeof(event.text));
    copyText(_bootstrapResponse->activation.code, event.code, sizeof(event.code));
    if (!publish(event)) return false;
    if (!pollActivation(generation, _bootstrapResponse->activation)) return false;
    clearCredentials();
    publishPhase(generation, AssistantPhase::Bootstrapping, "Refreshing credentials");
    return bootstrapUntilReady(generation);
  }
  if (!_bootstrapResponse->websocket.usable()) {
    publishError(generation, AssistantError::Bootstrap, true);
    clearCredentials();
    return false;
  }
  XiaozhiProtocol::ParsedUrl endpoint = {};
  if (!XiaozhiProtocol::parseWssUrl(_bootstrapResponse->websocket.url, endpoint)) {
    publishError(generation, AssistantError::Configuration, false);
    clearCredentials();
    return false;
  }
  _retryIndex = 0;
  if (!openConversation(generation)) return false;
  publishPhase(generation, AssistantPhase::Ready, "Ready");
  return true;
}

bool XiaozhiSession::waitForSecureTime(uint32_t generation) {
  const uint32_t started = millis();
  while (_time.status() != TimeStatus::Available) {
    if (interrupted(generation) || !_wifi.isConnected()) return false;
    if (millis() - started >= SecureTimeTimeoutMs) return false;
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  return true;
}

bool XiaozhiSession::pollActivation(
    uint32_t generation, const XiaozhiActivationInstructions &instructions) {
  uint32_t timeout = instructions.timeoutMs;
  if (timeout == 0) timeout = ActivationMaxMs;
  else if (timeout < ActivationMinMs) timeout = ActivationMinMs;
  if (timeout > ActivationMaxMs) timeout = ActivationMaxMs;
  const uint32_t started = millis();
  while (millis() - started < timeout) {
    if (!delayInterruptible(ActivationPollMs, generation)) return false;
    const XiaozhiActivationPoll result = _bootstrap.activate(_identity);
    if (result == XiaozhiActivationPoll::Accepted) return true;
    if (result == XiaozhiActivationPoll::FatalError) {
      publishError(generation, AssistantError::Activation, false);
      return false;
    }
    if (!_wifi.isConnected()) return false;
  }
  publishError(generation, AssistantError::Activation, true);
  return false;
}

bool XiaozhiSession::connectWebSocket(uint32_t generation) {
  XiaozhiProtocol::ParsedUrl parsed = {};
  if (!XiaozhiProtocol::parseWssUrl(_bootstrapResponse->websocket.url, parsed)) {
    publishError(generation, AssistantError::Configuration, false);
    return false;
  }
  char authorization[XiaozhiWebSocketCredentials::MaxTokenBytes + 8] = {};
  if (strchr(_bootstrapResponse->websocket.token, ' ')) {
    copyText(_bootstrapResponse->websocket.token, authorization, sizeof(authorization));
  } else {
    snprintf(authorization, sizeof(authorization), "Bearer %s",
             _bootstrapResponse->websocket.token);
  }
  char version[4] = {};
  snprintf(version, sizeof(version), "%u", _bootstrapResponse->websocket.version);
  const SecureWebSocketClient::Header headers[] = {
    {"Authorization", authorization},
    {"Protocol-Version", version},
    {"Device-Id", _identity.deviceId()},
    {"Client-Id", _identity.clientId()}
  };
  const SecureWebSocketClient::Endpoint endpoint = {parsed.host, parsed.port, parsed.path, ""};
  const SecureWebSocketClient::TrustStore trust = {
    SecureWebSocketClient::TrustKind::PemCertificate,
    reinterpret_cast<const uint8_t *>(_config.caCertificate),
    _config.caCertificateLength
  };
  const bool connected = _websocket.connect(
      endpoint, headers, sizeof(headers) / sizeof(headers[0]), trust);
  memset(authorization, 0, sizeof(authorization));
  if (!connected) {
    if (_websocket.lastFailure() == SecureWebSocketClient::FailureKind::AuthenticationRejected) {
      publishPhase(generation, AssistantPhase::Bootstrapping, "Refreshing credentials");
    } else {
      publishError(generation,
                   _websocket.lastFailure() == SecureWebSocketClient::FailureKind::UpgradeTimeout
                       ? AssistantError::TimeoutConnect : AssistantError::WebSocket,
                   true);
    }
  }
  return connected;
}

bool XiaozhiSession::openConversation(uint32_t generation) {
  memset(_sessionId, 0, sizeof(_sessionId));
  publishPhase(generation, AssistantPhase::Connecting, "Connecting");
  if (!connectWebSocket(generation)) return false;
  if (interrupted(generation)) {
    stopSession(false);
    return false;
  }
  char json[XiaozhiProtocol::MaxJsonBytes + 1] = {};
  const bool helloSent = XiaozhiProtocol::buildClientHello(
      _bootstrapResponse->websocket.version, json, sizeof(json)) &&
      _websocket.sendText(json, strlen(json));
  memset(json, 0, sizeof(json));
  if (!helloSent) {
    publishError(generation, AssistantError::Protocol, true);
    stopSession(false);
    return false;
  }

  const uint32_t started = millis();
  SecureWebSocketClient::Event event = {};
  while (!interrupted(generation) && millis() - started < HelloTimeoutMs) {
    if (!_wifi.isConnected()) {
      publishPhase(generation, AssistantPhase::NeedsWiFi, "Connect to Wi-Fi");
      stopSession(false);
      return false;
    }
    if (!_websocket.poll(event)) {
      vTaskDelay(1);
      continue;
    }
    if (event.type == SecureWebSocketClient::EventType::Text) {
      XiaozhiProtocol::Message message = {};
      if (!XiaozhiProtocol::parseMessage(event.payload, event.length, nullptr, message) ||
          message.type != XiaozhiProtocol::MessageType::ServerHello ||
          !message.sessionId[0]) {
        publishError(generation, AssistantError::Protocol, true);
        stopSession(false);
        return false;
      }
      copyText(message.sessionId, _sessionId, sizeof(_sessionId));
      if (!_encoder.begin() || !_decoder.begin(message.audio.sampleRate)) {
        publishError(generation, AssistantError::Codec, true);
        stopSession(false);
        return false;
      }
      _speaker.mute();
      _speaker.stop();
      _lastHeartbeatMs = millis();
      _heartbeatPending = false;
      return true;
    }
    if (event.type == SecureWebSocketClient::EventType::Error ||
        event.type == SecureWebSocketClient::EventType::Disconnected ||
        event.type == SecureWebSocketClient::EventType::Overflow) {
      publishError(generation, event.type == SecureWebSocketClient::EventType::Overflow
                                   ? AssistantError::QueueOverflow
                                   : AssistantError::WebSocket,
                   true);
      stopSession(false);
      return false;
    }
    memset(&event, 0, sizeof(event));
  }
  if (!interrupted(generation)) publishError(generation, AssistantError::TimeoutHello, true);
  stopSession(false);
  return false;
}

bool XiaozhiSession::runTurn(uint32_t generation) {
  clearAudioQueues();
  _receivedDownlinkPackets = 0;
  _receivedDownlinkBytes = 0;
  _playedPcmSamples = 0;
  _peakPcm = 0;
  _credentialsRejected = false;
  if (!_websocket.isConnected() || !_sessionId[0]) return false;
  _speaker.mute();
  _speaker.stop();
  if (!_mic.isPowered() || !_mic.start()) {
    publishError(generation, AssistantError::Microphone, true);
    return false;
  }
  char json[XiaozhiProtocol::MaxJsonBytes + 1] = {};
  if (!XiaozhiProtocol::buildListenStart(_sessionId, json, sizeof(json)) ||
      !_websocket.sendText(json, strlen(json))) {
    memset(json, 0, sizeof(json));
    publishError(generation, AssistantError::Protocol, true);
    stopTurn(false);
    return false;
  }
  memset(json, 0, sizeof(json));
  const uint32_t turnStarted = millis();
  uint32_t phaseStarted = turnStarted;
  uint32_t lastPacket = turnStarted;
  bool ttsStarted = false;
  bool ttsStopped = false;
  AssistantPhase phase = AssistantPhase::Listening;
  publishPhase(generation, phase, "Listening");
  size_t pcmUsed = 0;
  SecureWebSocketClient::Event event = {};

  while (!interrupted(generation)) {
    const uint32_t now = millis();
    if (!_wifi.isConnected()) {
      stopSession(false);
      publishPhase(generation, AssistantPhase::NeedsWiFi, "Connect to Wi-Fi");
      return false;
    }
    if (now - turnStarted >= TurnTimeoutMs) {
      publishError(generation, AssistantError::TimeoutTurn, true);
      stopTurn(false); return false;
    }
    if (phase == AssistantPhase::Listening && now - lastPacket >= ListeningTimeoutMs) {
      publishError(generation, AssistantError::TimeoutListening, true);
      stopTurn(false); return false;
    }
    if (phase == AssistantPhase::Thinking && now - phaseStarted >= ThinkingTimeoutMs) {
      publishError(generation, AssistantError::TimeoutThinking, true);
      stopTurn(false); return false;
    }
    if (phase == AssistantPhase::Speaking &&
        (now - lastPacket >= SpeakingPacketTimeoutMs ||
         now - phaseStarted >= SpeakingTotalTimeoutMs)) {
      publishError(generation,
                   now - phaseStarted >= SpeakingTotalTimeoutMs
                       ? AssistantError::TimeoutSpeakingTotal
                       : AssistantError::TimeoutSpeakingPacket,
                   true);
      stopTurn(false); return false;
    }

    if (_websocket.poll(event)) {
      if (event.type == SecureWebSocketClient::EventType::Text) {
        if (!handleText(event, generation, ttsStarted, ttsStopped, phase,
                        phaseStarted, lastPacket)) {
          memset(&event, 0, sizeof(event)); stopTurn(false); return false;
        }
      } else if (event.type == SecureWebSocketClient::EventType::Binary) {
        if (!ttsStarted || (phase != AssistantPhase::Thinking &&
                            phase != AssistantPhase::Speaking)) {
          publishError(generation, AssistantError::Protocol, true);
          memset(&event, 0, sizeof(event)); stopTurn(false); return false;
        }
        const uint8_t *opus = nullptr;
        size_t opusLength = 0;
        uint32_t timestamp = 0;
        if (!XiaozhiProtocol::decodeAudioEnvelope(_bootstrapResponse->websocket.version,
                                                   event.payload, event.length,
                                                   opus, opusLength, timestamp) ||
            !enqueueDownlink(generation, opus, opusLength, timestamp)) {
          publishError(generation, _downlinkCount >= DownlinkQueueCapacity
                                       ? AssistantError::QueueOverflow
                                       : AssistantError::Protocol, true);
          memset(&event, 0, sizeof(event)); stopTurn(false); return false;
        }
        if (phase == AssistantPhase::Thinking) {
          if (!_speaker.start(_decoder.sampleRate())) {
            publishError(generation, AssistantError::Speaker, true);
            memset(&event, 0, sizeof(event)); stopTurn(false); return false;
          }
          phase = AssistantPhase::Speaking;
          phaseStarted = now;
          publishPhase(generation, phase, "Speaking");
        }
        lastPacket = now;
      } else if (event.type == SecureWebSocketClient::EventType::Overflow) {
        publishError(generation, AssistantError::QueueOverflow, true);
        stopTurn(false); return false;
      } else if (event.type == SecureWebSocketClient::EventType::Error ||
                 event.type == SecureWebSocketClient::EventType::Disconnected) {
        publishError(generation, AssistantError::WebSocket, true);
        stopSession(false); return false;
      }
      memset(&event, 0, sizeof(event));
    }

    if (phase == AssistantPhase::Listening && !captureAndSend(generation, pcmUsed)) {
      if (interrupted(generation)) break;
      publishError(generation, _uplinkCount >= UplinkQueueCapacity
                                   ? AssistantError::QueueOverflow
                                   : AssistantError::Microphone,
                   true);
      stopTurn(false); return false;
    }
    if (phase == AssistantPhase::Speaking && _downlinkCount && !playOne(generation)) {
      if (interrupted(generation)) break;
      publishError(generation, AssistantError::Speaker, true);
      stopTurn(false); return false;
    }
    if (ttsStopped && _downlinkCount == 0) break;
    vTaskDelay(1);
  }
  if (interrupted(generation)) {
    sendAbort();
    stopTurn(false);
    return false;
  }
  if (ttsStopped && phase == AssistantPhase::Speaking && _receivedDownlinkPackets == 0) {
    publishError(generation, AssistantError::Protocol, true);
    stopTurn(false);
    return false;
  }
  stopTurn(true);
  _lastHeartbeatMs = millis();
  return true;
}

bool XiaozhiSession::handleText(const SecureWebSocketClient::Event &event,
                                uint32_t generation, bool &ttsStarted, bool &ttsStopped,
                                AssistantPhase &phase,
                                uint32_t &phaseStarted, uint32_t &lastPacket) {
  XiaozhiProtocol::Message message = {};
  if (!XiaozhiProtocol::parseMessage(event.payload, event.length,
                                     _sessionId, message)) {
    publishError(generation, AssistantError::Protocol, true);
    return false;
  }
  if (interrupted(generation)) return true;
  const uint32_t now = millis();
  if (message.type == XiaozhiProtocol::MessageType::DeviceCommand) {
    if (phase != AssistantPhase::Thinking || !handleDeviceCommand(message, generation)) {
      publishError(generation, AssistantError::Protocol, true);
      return false;
    }
  } else if (message.type == XiaozhiProtocol::MessageType::Stt) {
    // Xiaozhi can finish a second ASR task after the first final STT has
    // already advanced this half-duplex turn. It is stale at this point; do
    // not let it disable active TTS playback.
    if (phase != AssistantPhase::Listening) return true;
    _mic.stop();
    // The backend's auto VAD already completed this utterance. Sending a
    // second listen-stop starts another ASR job while the LLM tool runs.
    phase = AssistantPhase::Thinking;
    phaseStarted = lastPacket = now;
    publishCaption(generation, XiaozhiSessionEventType::SttCaption, message.text);
    publishPhase(generation, phase, "Thinking");
  } else if (message.type == XiaozhiProtocol::MessageType::TtsStart) {
    _mic.stop();
    // The backend sends this before LLM tool selection. Keep the turn in
    // Thinking so authenticated device commands can run; audio packets are
    // the point at which the speaker actually becomes active.
    ttsStarted = true;
  } else if (message.type == XiaozhiProtocol::MessageType::TtsCaption) {
    publishCaption(generation, XiaozhiSessionEventType::TtsCaption, message.text);
  } else if (message.type == XiaozhiProtocol::MessageType::Emotion) {
    XiaozhiSessionEvent output = {};
    output.type = XiaozhiSessionEventType::Emotion;
    output.generation = generation;
    output.emotion = message.emotion;
    publish(output);
  } else if (message.type == XiaozhiProtocol::MessageType::TtsStop) {
    ttsStopped = true;
  } else if (message.type == XiaozhiProtocol::MessageType::Pong) {
    _heartbeatPending = false;
    _lastHeartbeatMs = now;
  } else if (message.type == XiaozhiProtocol::MessageType::Goodbye ||
             message.type == XiaozhiProtocol::MessageType::ServerHello) {
    publishError(generation, AssistantError::WebSocket, true);
    return false;
  } else if (message.type == XiaozhiProtocol::MessageType::Error) {
    if (message.authenticationRejected) {
      _credentialsRejected = true;
      publishPhase(generation, AssistantPhase::Bootstrapping, "Refreshing credentials");
    } else {
      publishError(generation, AssistantError::Protocol, true);
    }
    return false;
  } else if (message.type == XiaozhiProtocol::MessageType::Invalid) {
    publishError(generation, AssistantError::Protocol, true);
    return false;
  }
  return true;
}

bool XiaozhiSession::handleDeviceCommand(const XiaozhiProtocol::Message &message,
                                         uint32_t generation) {
  if (!message.requestId || message.requestId == _lastDeviceCommandId || !_sessionId[0]) return false;
  _lastDeviceCommandId = message.requestId;
  Serial.print(F("Assistant device command generation="));
  Serial.print(generation);
  Serial.print(F(" name="));
  Serial.print(message.deviceCommand == XiaozhiProtocol::DeviceCommandKind::SearchAudio
                   ? F("search_audio") : F("play_audio"));
  Serial.print(F(" mounted="));
  Serial.print(_sdCard.isMounted() ? 1 : 0);
  Serial.print(F(" owner="));
  Serial.println(static_cast<uint8_t>(_sdCard.owner()));
  XiaozhiProtocol::DeviceToolResult result = {};
  result.clear();
  String launchPath;
  if (message.deviceCommand == XiaozhiProtocol::DeviceCommandKind::SearchAudio) {
    result = _deviceTools.searchAudio(message.text);
  } else if (message.deviceCommand == XiaozhiProtocol::DeviceCommandKind::PlayAudio) {
    result = _deviceTools.playAudio(message.selectionId, launchPath);
  } else {
    result.error = XiaozhiProtocol::DeviceToolError::Protocol;
  }

  Serial.print(F("Assistant device result generation="));
  Serial.print(generation);
  Serial.print(F(" ok="));
  Serial.print(result.ok ? 1 : 0);
  Serial.print(F(" error="));
  Serial.print(deviceToolErrorName(result.error));
  Serial.print(F(" matches="));
  Serial.print(result.matchCount);
  Serial.print(F(" accepted="));
  Serial.println(result.accepted ? 1 : 0);

  char json[XiaozhiProtocol::MaxJsonBytes + 1] = {};
  const bool sent = XiaozhiProtocol::buildDeviceResult(_sessionId, message.requestId,
                                                        result, json, sizeof(json)) &&
                    _websocket.sendText(json, strlen(json));
  memset(json, 0, sizeof(json));
  if (!sent) return false;
  if (result.ok && result.accepted && launchPath.length() &&
      launchPath.length() <= AssistantDeviceTools::MaxPathBytes) {
    XiaozhiSessionEvent event = {};
    event.type = XiaozhiSessionEventType::LaunchAudio;
    event.generation = generation;
    event.micAvailable = _mic.isReady() && _mic.isPowered();
    copyText(launchPath.c_str(), event.audioPath, sizeof(event.audioPath));
    if (!publish(event)) return false;
  }
  return true;
}

bool XiaozhiSession::resetConversation(uint32_t generation) {
  stopTurn(false);
  if (!_websocket.isConnected()) return false;
  char json[XiaozhiProtocol::MaxJsonBytes + 1] = {};
  const bool sent = XiaozhiProtocol::buildConversationReset(
      generation, json, sizeof(json)) &&
      _websocket.sendText(json, strlen(json));
  memset(json, 0, sizeof(json));
  if (!sent) {
    publishError(generation, AssistantError::ConversationReset, true);
    return false;
  }

  const uint32_t started = millis();
  SecureWebSocketClient::Event event = {};
  while (!interrupted(generation) && millis() - started < ConversationResetTimeoutMs) {
    if (!_websocket.poll(event)) {
      vTaskDelay(1);
      continue;
    }
    if (event.type == SecureWebSocketClient::EventType::Text) {
      XiaozhiProtocol::Message message = {};
      if (!XiaozhiProtocol::parseMessage(event.payload, event.length,
                                         _sessionId, message)) {
        publishError(generation, AssistantError::Protocol, true);
        return false;
      }
      if (message.type == XiaozhiProtocol::MessageType::Pong) {
        _heartbeatPending = false;
        _lastHeartbeatMs = millis();
      } else if (message.type == XiaozhiProtocol::MessageType::ConversationReset &&
                 message.requestId == generation) {
        if (!message.resetSucceeded) {
          publishError(generation, AssistantError::ConversationReset, true);
          return false;
        }
        return true;
      }
    } else if (event.type == SecureWebSocketClient::EventType::Error ||
               event.type == SecureWebSocketClient::EventType::Disconnected ||
               event.type == SecureWebSocketClient::EventType::Overflow) {
      publishError(generation, AssistantError::ConversationReset, true);
      return false;
    }
    memset(&event, 0, sizeof(event));
  }
  if (!interrupted(generation))
    publishError(generation, AssistantError::TimeoutConversationReset, true);
  return false;
}

bool XiaozhiSession::serviceIdleConnection(uint32_t generation) {
  if (!_websocket.isConnected()) return false;
  const uint32_t now = millis();
  if (_heartbeatPending && now - _heartbeatSentMs >= HeartbeatTimeoutMs) return false;
  if (!_heartbeatPending && now - _lastHeartbeatMs >= HeartbeatIntervalMs) {
    char json[48] = {};
    if (!XiaozhiProtocol::buildPing(json, sizeof(json)) ||
        !_websocket.sendText(json, strlen(json))) return false;
    _heartbeatPending = true;
    _heartbeatSentMs = now;
  }

  SecureWebSocketClient::Event event = {};
  if (!_websocket.poll(event)) return true;
  if (event.type == SecureWebSocketClient::EventType::Text) {
    XiaozhiProtocol::Message message = {};
    if (!XiaozhiProtocol::parseMessage(event.payload, event.length,
                                       _sessionId, message)) return false;
    if (message.type == XiaozhiProtocol::MessageType::Pong) {
      _heartbeatPending = false;
      _lastHeartbeatMs = now;
      return true;
    }
    // No unsolicited assistant instruction is actionable while idle.
    return message.type == XiaozhiProtocol::MessageType::Ignored;
  }
  if (event.type == SecureWebSocketClient::EventType::Pong) {
    _heartbeatPending = false;
    _lastHeartbeatMs = now;
    return true;
  }
  if (event.type == SecureWebSocketClient::EventType::Ping ||
      event.type == SecureWebSocketClient::EventType::Connected) return true;
  (void)generation;
  return event.type != SecureWebSocketClient::EventType::Error &&
         event.type != SecureWebSocketClient::EventType::Disconnected &&
         event.type != SecureWebSocketClient::EventType::Overflow;
}

bool XiaozhiSession::captureAndSend(uint32_t generation, size_t &pcmUsed) {
  if (!_mic.isRecording()) return false;
  if (_uplinkCount == UplinkQueueCapacity) return false;
  PcmFrame &fill = _uplink[(_uplinkHead + _uplinkCount) % UplinkQueueCapacity];
  const size_t received = _mic.readPcm16(
      fill.samples + pcmUsed, AssistantAudio::OpusEncoder::FrameSamples - pcmUsed);
  if (!received) return true;
  pcmUsed += received;
  if (pcmUsed < AssistantAudio::OpusEncoder::FrameSamples) return true;
  fill.generation = generation;
  ++_uplinkCount;
  pcmUsed = 0;

  PcmFrame &frame = _uplink[_uplinkHead];
  if (frame.generation != generation || interrupted(generation)) {
    memset(&frame, 0, sizeof(frame));
    _uplinkHead = (_uplinkHead + 1) % UplinkQueueCapacity;
    --_uplinkCount;
    return true;
  }
  uint8_t opus[XiaozhiProtocol::MaxAudioPacketBytes] = {};
  const int encoded = _encoder.encode(frame.samples, AssistantAudio::OpusEncoder::FrameSamples,
                                      opus, sizeof(opus));
  if (encoded <= 0) { memset(opus, 0, sizeof(opus)); return false; }
  uint8_t envelope[AudioEnvelopeBytes] = {};
  size_t envelopeLength = 0;
  const bool wrapped = XiaozhiProtocol::encodeAudioEnvelope(
      _bootstrapResponse->websocket.version, opus, static_cast<size_t>(encoded),
      millis(), envelope, sizeof(envelope), envelopeLength);
  memset(opus, 0, sizeof(opus));
  const bool sent = wrapped && !interrupted(generation) &&
                    _websocket.sendBinary(envelope, envelopeLength);
  memset(envelope, 0, sizeof(envelope));
  memset(&frame, 0, sizeof(frame));
  _uplinkHead = (_uplinkHead + 1) % UplinkQueueCapacity;
  --_uplinkCount;
  return sent;
}

bool XiaozhiSession::enqueueDownlink(uint32_t generation, const uint8_t *bytes,
                                     size_t length, uint32_t timestamp) {
  if (!bytes || !length || length > XiaozhiProtocol::MaxAudioPacketBytes ||
      _downlinkCount == DownlinkQueueCapacity) return false;
  OpusPacket &packet = _downlink[(_downlinkHead + _downlinkCount) % DownlinkQueueCapacity];
  packet.generation = generation;
  packet.timestamp = timestamp;
  packet.length = static_cast<uint16_t>(length);
  memcpy(packet.bytes, bytes, length);
  ++_downlinkCount;
  ++_receivedDownlinkPackets;
  _receivedDownlinkBytes += static_cast<uint32_t>(length);
  return true;
}

bool XiaozhiSession::playOne(uint32_t generation) {
  OpusPacket &packet = _downlink[_downlinkHead];
  if (packet.generation != generation) {
    memset(&packet, 0, sizeof(packet));
    _downlinkHead = (_downlinkHead + 1) % DownlinkQueueCapacity;
    --_downlinkCount;
    return true;
  }
  const size_t capacity = _decoder.frameSamples();
  if (!_decodedPcm || capacity > AssistantAudio::OpusDecoder::MaxFrameSamples) return false;
  const int decoded = _decoder.decode(packet.bytes, packet.length, _decodedPcm, capacity);
  if (decoded > 0) {
    for (int index = 0; index < decoded; ++index) {
      const int32_t sample = _decodedPcm[index];
      const uint16_t magnitude = static_cast<uint16_t>(sample < 0 ? -sample : sample);
      if (magnitude > _peakPcm) _peakPcm = magnitude;
    }
    _playedPcmSamples += static_cast<uint32_t>(decoded);
  }
  const bool played = decoded > 0 && !interrupted(generation) &&
      _speaker.writePcm16(_decodedPcm, static_cast<size_t>(decoded), 250) ==
          static_cast<size_t>(decoded);
  memset(_decodedPcm, 0, capacity * sizeof(int16_t));
  memset(&packet, 0, sizeof(packet));
  _downlinkHead = (_downlinkHead + 1) % DownlinkQueueCapacity;
  --_downlinkCount;
  return played;
}

void XiaozhiSession::stopTurn(bool drainPlayback) {
  _mic.stop();
  if (drainPlayback) _speaker.drain();
  _speaker.mute();
  _speaker.stop();
  if (_receivedDownlinkPackets) {
    Serial.print(F("Assistant playback generation="));
    Serial.print(_generation);
    Serial.print(F(" packets="));
    Serial.print(_receivedDownlinkPackets);
    Serial.print(F(" opus_bytes="));
    Serial.print(_receivedDownlinkBytes);
    Serial.print(F(" pcm_samples="));
    Serial.print(_playedPcmSamples);
    Serial.print(F(" pcm_peak="));
    Serial.println(_peakPcm);
  }
  _receivedDownlinkPackets = 0;
  _receivedDownlinkBytes = 0;
  _playedPcmSamples = 0;
  _peakPcm = 0;
  clearAudioQueues();
}

void XiaozhiSession::stopSession(bool drainPlayback) {
  stopTurn(drainPlayback);
  _deviceTools.clearSelections();
  _lastDeviceCommandId = 0;
  _websocket.close();
  _encoder.end();
  _decoder.end();
  memset(_sessionId, 0, sizeof(_sessionId));
  _heartbeatPending = false;
  _heartbeatSentMs = 0;
  _lastHeartbeatMs = 0;
}

void XiaozhiSession::sendAbort() {
  if (!_websocket.isConnected()) return;
  char json[XiaozhiProtocol::MaxJsonBytes + 1] = {};
  if (XiaozhiProtocol::buildAbort(_sessionId, json, sizeof(json))) {
    _websocket.sendText(json, strlen(json));
  }
  memset(json, 0, sizeof(json));
}

void XiaozhiSession::clearAudioQueues() {
  if (_uplink) memset(_uplink, 0, sizeof(PcmFrame) * UplinkQueueCapacity);
  if (_downlink) memset(_downlink, 0, sizeof(OpusPacket) * DownlinkQueueCapacity);
  _uplinkHead = _uplinkCount = _downlinkHead = _downlinkCount = 0;
}

void XiaozhiSession::clearCredentials() {
  if (_bootstrapResponse) _bootstrapResponse->clear();
}

bool XiaozhiSession::interrupted(uint32_t generation) const {
  return _deactivateRequested.load(std::memory_order_acquire) ||
         _cancelRequested.load(std::memory_order_acquire) ||
         generation != _requestedGeneration.load(std::memory_order_acquire) ||
         !_screenActive;
}

bool XiaozhiSession::delayInterruptible(uint32_t delayMs, uint32_t generation) {
  const uint32_t started = millis();
  while (millis() - started < delayMs) {
    if (interrupted(generation)) return false;
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  return true;
}

void XiaozhiSession::publishPhase(uint32_t generation, AssistantPhase phase,
                                  const char *status) {
  XiaozhiSessionEvent event = {};
  event.type = XiaozhiSessionEventType::Phase;
  event.generation = generation;
  event.phase = phase;
  event.micAvailable = _mic.isReady() && _mic.isPowered();
  copyText(status, event.text, sizeof(event.text));
  publish(event);
  logTransition(generation, phase);
}

void XiaozhiSession::publishCaption(uint32_t generation, XiaozhiSessionEventType type,
                                    const char *caption) {
  size_t byteCount = 0;
  size_t nonAsciiBytes = 0;
  if (caption) {
    for (; caption[byteCount] && byteCount < AssistantSnapshot::CaptionBytes; ++byteCount) {
      if (static_cast<uint8_t>(caption[byteCount]) >= 0x80) ++nonAsciiBytes;
    }
  }
  Serial.print(F("Assistant caption generation="));
  Serial.print(generation);
  Serial.print(F(" kind="));
  Serial.print(type == XiaozhiSessionEventType::SttCaption ? F("stt") : F("tts"));
  Serial.print(F(" bytes="));
  Serial.print(byteCount);
  Serial.print(F(" non_ascii_bytes="));
  Serial.println(nonAsciiBytes);
  XiaozhiSessionEvent event = {};
  event.type = type;
  event.generation = generation;
  XiaozhiProtocol::copyUtf8Bounded(caption, event.text, sizeof(event.text));
  publish(event);
}

void XiaozhiSession::publishError(uint32_t generation, AssistantError error,
                                  bool retryable) {
  if (!retryable) _fatalError = true;
  XiaozhiSessionEvent event = {};
  event.type = XiaozhiSessionEventType::Error;
  event.generation = generation;
  event.phase = AssistantPhase::Error;
  event.error = error;
  event.retryable = retryable;
  event.micAvailable = _mic.isReady() && _mic.isPowered();
  publish(event);
  Serial.print(F("Assistant error generation="));
  Serial.print(generation);
  Serial.print(F(" category="));
  Serial.println(assistantErrorName(error));
}

void XiaozhiSession::publishDeactivated(uint32_t generation) {
  XiaozhiSessionEvent event = {};
  event.type = XiaozhiSessionEventType::Deactivated;
  event.generation = generation;
  publish(event);
}

bool XiaozhiSession::publish(const XiaozhiSessionEvent &event) {
  if (xQueueSend(_events, &event, 0) == pdTRUE) return true;
  // Captions and emotion are advisory UI updates. Dropping one is preferable
  // to closing the authenticated audio session merely because the renderer
  // has not reached its next tick yet.
  if (event.type == XiaozhiSessionEventType::SttCaption ||
      event.type == XiaozhiSessionEventType::TtsCaption ||
      event.type == XiaozhiSessionEventType::Emotion) return true;

  // Playback launch, teardown, and errors are control-plane events. Let the
  // UI drain the bounded PSRAM queue before conceding delivery; this worker
  // never draws or navigates directly.
  return xQueueSend(_events, &event, pdMS_TO_TICKS(250)) == pdTRUE;
}

void XiaozhiSession::logTransition(uint32_t generation, AssistantPhase phase) {
  Serial.print(F("Assistant phase generation="));
  Serial.print(generation);
  Serial.print(F(" phase="));
  Serial.print(assistantPhaseName(phase));
  Serial.print(F(" queues="));
  Serial.print(_uplinkCount);
  Serial.print('/');
  Serial.print(_downlinkCount);
  Serial.print(F(" heap="));
  Serial.print(ESP.getFreeHeap());
  Serial.print(F(" psram="));
  Serial.print(ESP.getFreePsram());
  // ESP-IDF configures FreeRTOS task stack depth in bytes on this target.
  Serial.print(F(" stack_free_bytes="));
  Serial.println(uxTaskGetStackHighWaterMark(nullptr));
}
