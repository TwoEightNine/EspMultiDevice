#include "AssistantState.h"

#include <string.h>

void AssistantSnapshot::clear() {
  memset(this, 0, sizeof(*this));
  phase = AssistantPhase::NeedsWiFi;
  error = AssistantError::None;
  emotion = AssistantEmotion::Neutral;
}

const char *assistantPhaseName(AssistantPhase phase) {
  switch (phase) {
    case AssistantPhase::NeedsWiFi: return "needs_wifi";
    case AssistantPhase::Bootstrapping: return "bootstrapping";
    case AssistantPhase::NeedsActivation: return "needs_activation";
    case AssistantPhase::Ready: return "ready";
    case AssistantPhase::Connecting: return "connecting";
    case AssistantPhase::Listening: return "listening";
    case AssistantPhase::Thinking: return "thinking";
    case AssistantPhase::Speaking: return "speaking";
    case AssistantPhase::Error: return "error";
  }
  return "unknown";
}

const char *assistantErrorName(AssistantError error) {
  switch (error) {
    case AssistantError::None: return "none";
    case AssistantError::Configuration: return "configuration";
    case AssistantError::Identity: return "identity";
    case AssistantError::SecureTime: return "secure_time";
    case AssistantError::Bootstrap: return "bootstrap";
    case AssistantError::Activation: return "activation";
    case AssistantError::Authentication: return "authentication";
    case AssistantError::WebSocket: return "websocket";
    case AssistantError::Protocol: return "protocol";
    case AssistantError::Microphone: return "microphone";
    case AssistantError::Speaker: return "speaker";
    case AssistantError::Codec: return "codec";
    case AssistantError::QueueOverflow: return "queue_overflow";
    case AssistantError::TimeoutBootstrap: return "bootstrap_timeout";
    case AssistantError::TimeoutConnect: return "connect_timeout";
    case AssistantError::TimeoutHello: return "hello_timeout";
    case AssistantError::TimeoutListening: return "listening_timeout";
    case AssistantError::TimeoutThinking: return "thinking_timeout";
    case AssistantError::TimeoutSpeakingPacket: return "speaking_packet_timeout";
    case AssistantError::TimeoutSpeakingTotal: return "speaking_total_timeout";
    case AssistantError::TimeoutTurn: return "turn_timeout";
    case AssistantError::TimeoutConversationReset: return "conversation_reset_timeout";
    case AssistantError::ConversationReset: return "conversation_reset";
    case AssistantError::Network: return "network";
    case AssistantError::Internal: return "internal";
  }
  return "unknown";
}

bool assistantTransitionAllowed(AssistantPhase from, AssistantPhase to) {
  if (to == AssistantPhase::NeedsWiFi) return true;
  if (from == AssistantPhase::NeedsWiFi) return to == AssistantPhase::Bootstrapping;
  if (from == AssistantPhase::Bootstrapping) {
    return to == AssistantPhase::Connecting || to == AssistantPhase::NeedsActivation ||
           to == AssistantPhase::Error;
  }
  if (from == AssistantPhase::NeedsActivation) {
    return to == AssistantPhase::Bootstrapping || to == AssistantPhase::Ready ||
           to == AssistantPhase::Error;
  }
  if (from == AssistantPhase::Ready) {
    return to == AssistantPhase::Listening || to == AssistantPhase::Connecting ||
           to == AssistantPhase::Error;
  }
  if (from == AssistantPhase::Connecting) {
    return to == AssistantPhase::Ready ||
           to == AssistantPhase::Bootstrapping || to == AssistantPhase::Error;
  }
  if (from == AssistantPhase::Listening) {
    return to == AssistantPhase::Thinking || to == AssistantPhase::Speaking ||
           to == AssistantPhase::Ready || to == AssistantPhase::Error;
  }
  if (from == AssistantPhase::Thinking) {
    return to == AssistantPhase::Speaking || to == AssistantPhase::Ready ||
           to == AssistantPhase::Error;
  }
  if (from == AssistantPhase::Speaking) {
    return to == AssistantPhase::Ready || to == AssistantPhase::Error;
  }
  if (from == AssistantPhase::Error) {
    return to == AssistantPhase::Bootstrapping || to == AssistantPhase::Ready;
  }
  return false;
}

bool assistantGenerationMatches(uint32_t current, uint32_t candidate) {
  return current != 0 && current == candidate;
}
