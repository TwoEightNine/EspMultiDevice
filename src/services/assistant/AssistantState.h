#pragma once

#include <Arduino.h>

enum class AssistantPhase : uint8_t {
  NeedsWiFi,
  Bootstrapping,
  NeedsActivation,
  Ready,
  Connecting,
  Listening,
  Thinking,
  Speaking,
  Error
};

enum class AssistantError : uint8_t {
  None,
  Configuration,
  Identity,
  SecureTime,
  Bootstrap,
  Activation,
  Authentication,
  WebSocket,
  Protocol,
  Microphone,
  Speaker,
  Codec,
  QueueOverflow,
  TimeoutBootstrap,
  TimeoutConnect,
  TimeoutHello,
  TimeoutListening,
  TimeoutThinking,
  TimeoutSpeakingPacket,
  TimeoutSpeakingTotal,
  TimeoutTurn,
  Network,
  Internal
};

enum class AssistantEmotion : uint8_t {
  Neutral,
  Happy,
  Sad,
  Angry,
  Surprised,
  Thinking,
  Unknown
};

struct AssistantSnapshot {
  static constexpr size_t StatusBytes = 72;
  static constexpr size_t ActivationCodeBytes = 32;
  static constexpr size_t ActivationMessageBytes = 160;
  static constexpr size_t CaptionBytes = 192;

  uint32_t revision;
  uint32_t generation;
  AssistantPhase phase;
  AssistantError error;
  AssistantEmotion emotion;
  bool retryable;
  bool micAvailable;
  bool sessionActive;
  char status[StatusBytes + 1];
  char activationCode[ActivationCodeBytes + 1];
  char activationMessage[ActivationMessageBytes + 1];
  char sttCaption[CaptionBytes + 1];
  char ttsCaption[CaptionBytes + 1];

  void clear();
};

const char *assistantPhaseName(AssistantPhase phase);
const char *assistantErrorName(AssistantError error);
bool assistantTransitionAllowed(AssistantPhase from, AssistantPhase to);
bool assistantGenerationMatches(uint32_t current, uint32_t candidate);
