#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "AssistantIdentity.h"
#include "AssistantState.h"
#include "SecureWebSocketClient.h"
#include "XiaozhiBootstrap.h"
#include "XiaozhiProtocol.h"
#include "../audio/OpusDecoder.h"
#include "../audio/OpusEncoder.h"

class MicController;
class SpeakerController;
class TimeController;
class WiFiController;

enum class XiaozhiSessionCommandType : uint8_t {
  Activate,
  Deactivate,
  StartTurn,
  CancelTurn,
  ResetConversation,
  Retry
};

struct XiaozhiSessionCommand {
  XiaozhiSessionCommandType type;
  uint32_t generation;
};

enum class XiaozhiSessionEventType : uint8_t {
  Phase,
  Activation,
  SttCaption,
  TtsCaption,
  Emotion,
  ConversationReset,
  ContextLost,
  Error,
  Deactivated
};

struct XiaozhiSessionEvent {
  XiaozhiSessionEventType type;
  uint32_t generation;
  AssistantPhase phase;
  AssistantError error;
  AssistantEmotion emotion;
  bool retryable;
  bool micAvailable;
  char text[AssistantSnapshot::CaptionBytes + 1];
  char code[AssistantSnapshot::ActivationCodeBytes + 1];
};

class XiaozhiSession {
public:
  static constexpr UBaseType_t CommandQueueCapacity = 4;
  static constexpr UBaseType_t EventQueueCapacity = 8;
  static constexpr size_t UplinkQueueCapacity = 4;
  static constexpr size_t DownlinkQueueCapacity = 8;
  static constexpr uint32_t WorkerStackBytes = 48U * 1024U;

  XiaozhiSession(WiFiController &wifi, TimeController &time, MicController &mic,
                 SpeakerController &speaker, const AssistantBackendConfig &config);
  ~XiaozhiSession();

  bool begin();
  bool post(const XiaozhiSessionCommand &command);
  bool poll(XiaozhiSessionEvent &event);
  bool running() const;

private:
  struct PcmFrame {
    uint32_t generation;
    int16_t samples[AssistantAudio::OpusEncoder::FrameSamples];
  };
  struct OpusPacket {
    uint32_t generation;
    uint32_t timestamp;
    uint16_t length;
    uint8_t bytes[AssistantAudio::OpusDecoder::MaxPacketBytes];
  };

  WiFiController &_wifi;
  TimeController &_time;
  MicController &_mic;
  SpeakerController &_speaker;
  AssistantIdentity _identity;
  XiaozhiBootstrap _bootstrap;
  AssistantBackendConfig _config;
  SecureWebSocketClient _websocket;
  AssistantAudio::OpusEncoder _encoder;
  AssistantAudio::OpusDecoder _decoder;
  // Session credentials and queue payloads are bounded but large enough to
  // matter to mbedTLS's internal-heap handshake allocations. Keep them in
  // PSRAM; only the FreeRTOS queue control blocks remain internal.
  XiaozhiBootstrapResponse *_bootstrapResponse;
  QueueHandle_t _commands;
  QueueHandle_t _events;
  uint8_t *_commandStorage;
  uint8_t *_eventStorage;
  StaticQueue_t _commandQueueControl;
  StaticQueue_t _eventQueueControl;
  TaskHandle_t _task;
  std::atomic<uint32_t> _requestedGeneration;
  std::atomic<bool> _cancelRequested;
  std::atomic<bool> _deactivateRequested;
  std::atomic<bool> _workerExited;
  bool _screenActive;
  bool _fatalError;
  bool _credentialsRejected;
  uint32_t _generation;
  uint8_t _retryIndex;
  uint32_t _lastHeartbeatMs;
  uint32_t _heartbeatSentMs;
  bool _heartbeatPending;
  char _sessionId[XiaozhiProtocol::MaxSessionIdBytes + 1];
  PcmFrame *_uplink;
  OpusPacket *_downlink;
  int16_t *_decodedPcm;
  size_t _uplinkHead;
  size_t _uplinkCount;
  size_t _downlinkHead;
  size_t _downlinkCount;
  uint16_t _receivedDownlinkPackets;
  uint32_t _receivedDownlinkBytes;
  uint32_t _playedPcmSamples;
  uint16_t _peakPcm;

  static void workerEntry(void *context);
  void worker();
  void handle(const XiaozhiSessionCommand &command);
  bool bootstrapUntilReady(uint32_t generation);
  bool waitForSecureTime(uint32_t generation);
  bool pollActivation(uint32_t generation,
                      const XiaozhiActivationInstructions &instructions);
  bool runTurn(uint32_t generation);
  bool connectWebSocket(uint32_t generation);
  bool openConversation(uint32_t generation);
  bool resetConversation(uint32_t generation);
  bool serviceIdleConnection(uint32_t generation);
  bool handleText(const SecureWebSocketClient::Event &event, uint32_t generation,
                  bool &ttsStopped, AssistantPhase &phase,
                  uint32_t &phaseStarted, uint32_t &lastPacket);
  bool captureAndSend(uint32_t generation, size_t &pcmUsed);
  bool playOne(uint32_t generation);
  bool enqueueDownlink(uint32_t generation, const uint8_t *bytes, size_t length,
                       uint32_t timestamp);
  void stopTurn(bool drainPlayback);
  void stopSession(bool drainPlayback);
  void sendAbort();
  void clearAudioQueues();
  void clearCredentials();
  bool interrupted(uint32_t generation) const;
  bool delayInterruptible(uint32_t delayMs, uint32_t generation);
  void publishPhase(uint32_t generation, AssistantPhase phase, const char *status);
  void publishCaption(uint32_t generation, XiaozhiSessionEventType type,
                      const char *caption);
  void publishError(uint32_t generation, AssistantError error, bool retryable);
  void publishDeactivated(uint32_t generation);
  bool publish(const XiaozhiSessionEvent &event);
  void logTransition(uint32_t generation, AssistantPhase phase);
};
