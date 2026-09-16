#pragma once

#include <Arduino.h>

#include "AssistantState.h"

namespace XiaozhiProtocol {

constexpr size_t MaxHostBytes = 253;
constexpr size_t MaxPathBytes = 511;
constexpr size_t MaxJsonBytes = 2048;
constexpr size_t MaxSessionIdBytes = 96;
constexpr size_t MaxAudioPacketBytes = 1275;

struct ParsedUrl {
  char host[MaxHostBytes + 1];
  char path[MaxPathBytes + 1];
  uint16_t port;
};

struct AudioFormat {
  uint32_t sampleRate;
  uint8_t channels;
  uint8_t frameDurationMs;
};

enum class MessageType : uint8_t {
  Ignored,
  ServerHello,
  Stt,
  TtsStart,
  TtsStop,
  TtsCaption,
  Emotion,
  Goodbye,
  Error,
  Invalid
};

struct Message {
  MessageType type;
  AudioFormat audio;
  AssistantEmotion emotion;
  bool authenticationRejected;
  char sessionId[MaxSessionIdBytes + 1];
  char text[AssistantSnapshot::CaptionBytes + 1];
  void clear();
};

bool parseHttpsUrl(const char *url, ParsedUrl &output);
bool parseWssUrl(const char *url, ParsedUrl &output);
bool buildClientHello(uint8_t version, char *output, size_t capacity);
bool buildListenStart(const char *sessionId, char *output, size_t capacity);
bool buildListenStop(const char *sessionId, char *output, size_t capacity);
bool buildAbort(const char *sessionId, char *output, size_t capacity);
bool parseMessage(const uint8_t *json, size_t length, const char *expectedSessionId,
                  Message &message);
bool encodeAudioEnvelope(uint8_t version, const uint8_t *opus, size_t opusLength,
                         uint32_t timestamp, uint8_t *output, size_t capacity,
                         size_t &outputLength);
bool decodeAudioEnvelope(uint8_t version, const uint8_t *input, size_t inputLength,
                         const uint8_t *&opus, size_t &opusLength,
                         uint32_t &timestamp);

}  // namespace XiaozhiProtocol
