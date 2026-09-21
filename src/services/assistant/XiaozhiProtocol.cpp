#include "XiaozhiProtocol.h"

#include <cJSON.h>
#include <ctype.h>
#include <string.h>

namespace {

bool copyBounded(const char *value, char *output, size_t capacity) {
  if (!output || capacity == 0) return false;
  output[0] = 0;
  if (!value) return true;
  const size_t length = strnlen(value, capacity);
  const size_t copyLength = length < capacity ? length : capacity - 1;
  memcpy(output, value, copyLength);
  output[copyLength] = 0;
  return length < capacity;
}

bool parsePort(const char *value, size_t length, uint16_t &port) {
  if (!value || length == 0 || length > 5) return false;
  uint32_t parsed = 0;
  for (size_t i = 0; i < length; ++i) {
    if (!isdigit(static_cast<unsigned char>(value[i]))) return false;
    parsed = parsed * 10U + static_cast<uint32_t>(value[i] - '0');
  }
  if (parsed == 0 || parsed > UINT16_MAX) return false;
  port = static_cast<uint16_t>(parsed);
  return true;
}

bool validDnsHost(const char *host, size_t length) {
  if (!host || length == 0 || length > XiaozhiProtocol::MaxHostBytes ||
      host[0] == '.' || host[length - 1] == '.') return false;
  bool hasAlpha = false;
  size_t labelLength = 0;
  for (size_t i = 0; i < length; ++i) {
    const char c = host[i];
    if (c == '.') {
      if (labelLength == 0 || host[i - 1] == '-') return false;
      labelLength = 0;
      continue;
    }
    if (!(isalnum(static_cast<unsigned char>(c)) || c == '-')) return false;
    if (labelLength == 0 && c == '-') return false;
    if (isalpha(static_cast<unsigned char>(c))) hasAlpha = true;
    if (++labelLength > 63) return false;
  }
  return labelLength > 0 && host[length - 1] != '-' && hasAlpha;
}

bool parseSecureUrl(const char *url, const char *scheme, uint16_t defaultPort,
                    XiaozhiProtocol::ParsedUrl &output) {
  memset(&output, 0, sizeof(output));
  if (!url || !scheme) return false;
  const size_t schemeLength = strlen(scheme);
  const size_t urlLength = strnlen(url, 1025);
  if (urlLength == 0 || urlLength > 1024 ||
      strncmp(url, scheme, schemeLength) != 0 || strchr(url, '#')) return false;
  const char *authority = url + schemeLength;
  if (!*authority || strchr(authority, '@') || *authority == '[') return false;
  const char *authorityEnd = strpbrk(authority, "/?");
  if (!authorityEnd) authorityEnd = url + urlLength;
  const char *colon = nullptr;
  for (const char *cursor = authority; cursor < authorityEnd; ++cursor) {
    if (*cursor == ':') {
      if (colon) return false;
      colon = cursor;
    }
  }
  const char *hostEnd = colon ? colon : authorityEnd;
  const size_t hostLength = static_cast<size_t>(hostEnd - authority);
  if (!validDnsHost(authority, hostLength)) return false;
  memcpy(output.host, authority, hostLength);
  output.host[hostLength] = 0;
  output.port = defaultPort;
  if (colon && !parsePort(colon + 1, static_cast<size_t>(authorityEnd - colon - 1),
                          output.port)) return false;
  const char *path = authorityEnd;
  if (!*path) {
    strcpy(output.path, "/");
    return true;
  }
  if (*path == '?') {
    output.path[0] = '/';
    const size_t length = strlen(path);
    if (length + 1 > XiaozhiProtocol::MaxPathBytes) return false;
    memcpy(output.path + 1, path, length + 1);
  } else {
    const size_t length = strlen(path);
    if (length == 0 || length > XiaozhiProtocol::MaxPathBytes) return false;
    memcpy(output.path, path, length + 1);
  }
  for (const char *cursor = output.path; *cursor; ++cursor) {
    const unsigned char c = static_cast<unsigned char>(*cursor);
    if (c <= 0x20 || c == 0x7f || c == '\\') return false;
  }
  return true;
}

bool serialize(cJSON *root, char *output, size_t capacity) {
  if (!root || !output || capacity < 2) return false;
  const bool ok = cJSON_PrintPreallocated(root, output, static_cast<int>(capacity), false);
  cJSON_Delete(root);
  if (!ok) output[0] = 0;
  return ok;
}

cJSON *baseMessage(const char *type, const char *sessionId) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return nullptr;
  if (sessionId && sessionId[0]) cJSON_AddStringToObject(root, "session_id", sessionId);
  cJSON_AddStringToObject(root, "type", type);
  return root;
}

bool numberEquals(cJSON *object, const char *name, int expected) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
  return !item || (cJSON_IsNumber(item) && item->valueint == expected);
}

uint16_t read16(const uint8_t *p) {
  return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

uint32_t read32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

void write16(uint8_t *p, uint16_t value) {
  p[0] = static_cast<uint8_t>(value >> 8);
  p[1] = static_cast<uint8_t>(value);
}

void write32(uint8_t *p, uint32_t value) {
  p[0] = static_cast<uint8_t>(value >> 24);
  p[1] = static_cast<uint8_t>(value >> 16);
  p[2] = static_cast<uint8_t>(value >> 8);
  p[3] = static_cast<uint8_t>(value);
}

AssistantEmotion parseEmotion(const char *value) {
  if (!value) return AssistantEmotion::Unknown;
  if (!strcmp(value, "neutral")) return AssistantEmotion::Neutral;
  if (!strcmp(value, "happy")) return AssistantEmotion::Happy;
  if (!strcmp(value, "sad")) return AssistantEmotion::Sad;
  if (!strcmp(value, "angry")) return AssistantEmotion::Angry;
  if (!strcmp(value, "surprised")) return AssistantEmotion::Surprised;
  if (!strcmp(value, "thinking")) return AssistantEmotion::Thinking;
  return AssistantEmotion::Unknown;
}

bool isAuthenticationError(cJSON *root) {
  cJSON *code = cJSON_GetObjectItemCaseSensitive(root, "code");
  if (!code) code = cJSON_GetObjectItemCaseSensitive(root, "status");
  if (cJSON_IsNumber(code)) return code->valueint == 401 || code->valueint == 403;
  if (!cJSON_IsString(code) || !code->valuestring) return false;
  static constexpr const char *KnownCodes[] = {
    "401", "403", "unauthorized", "forbidden", "authentication_failed",
    "invalid_token", "token_expired"
  };
  for (const char *known : KnownCodes) {
    if (!strcmp(code->valuestring, known)) return true;
  }
  return false;
}

}  // namespace

namespace XiaozhiProtocol {

bool copyUtf8Bounded(const char *value, char *output, size_t capacity) {
  if (!output || capacity == 0) return false;
  output[0] = 0;
  if (!value) return true;
  const size_t limit = capacity - 1;
  size_t input = 0;
  size_t written = 0;
  bool complete = true;
  while (value[input]) {
    const uint8_t first = static_cast<uint8_t>(value[input]);
    uint8_t count = first < 0x80 ? 1 :
                    (first >= 0xc2 && first <= 0xdf ? 2 :
                    (first >= 0xe0 && first <= 0xef ? 3 :
                    (first >= 0xf0 && first <= 0xf4 ? 4 : 0)));
    bool valid = count != 0;
    for (uint8_t offset = 1; valid && offset < count; ++offset) {
      if (!value[input + offset] ||
          (static_cast<uint8_t>(value[input + offset]) & 0xc0) != 0x80) valid = false;
    }
    if (valid && count == 3) {
      const uint8_t second = static_cast<uint8_t>(value[input + 1]);
      if ((first == 0xe0 && second < 0xa0) ||
          (first == 0xed && second > 0x9f)) valid = false;
    } else if (valid && count == 4) {
      const uint8_t second = static_cast<uint8_t>(value[input + 1]);
      if ((first == 0xf0 && second < 0x90) ||
          (first == 0xf4 && second > 0x8f)) valid = false;
    }
    if (!valid) {
      if (written == limit) { complete = false; break; }
      output[written++] = '?';
      ++input;
      continue;
    }
    if (written + count > limit) { complete = false; break; }
    memcpy(output + written, value + input, count);
    written += count;
    input += count;
  }
  output[written] = 0;
  return complete && value[input] == 0;
}

void Message::clear() {
  memset(this, 0, sizeof(*this));
  type = MessageType::Invalid;
  audio = {16000, 1, 60};
  emotion = AssistantEmotion::Neutral;
}

bool parseHttpsUrl(const char *url, ParsedUrl &output) {
  return parseSecureUrl(url, "https://", 443, output);
}

bool parseWssUrl(const char *url, ParsedUrl &output) {
  return parseSecureUrl(url, "wss://", 443, output);
}

bool buildClientHello(uint8_t version, char *output, size_t capacity) {
  cJSON *root = baseMessage("hello", nullptr);
  if (!root) return false;
  cJSON_AddNumberToObject(root, "version", version);
  cJSON *features = cJSON_AddObjectToObject(root, "features");
  cJSON_AddBoolToObject(features, "mcp", false);
  cJSON_AddStringToObject(root, "transport", "websocket");
  cJSON *audio = cJSON_AddObjectToObject(root, "audio_params");
  cJSON_AddStringToObject(audio, "format", "opus");
  cJSON_AddNumberToObject(audio, "sample_rate", 16000);
  cJSON_AddNumberToObject(audio, "channels", 1);
  cJSON_AddNumberToObject(audio, "frame_duration", 60);
  return serialize(root, output, capacity);
}

bool buildListenStart(const char *sessionId, char *output, size_t capacity) {
  cJSON *root = baseMessage("listen", sessionId);
  if (!root) return false;
  cJSON_AddStringToObject(root, "state", "start");
  cJSON_AddStringToObject(root, "mode", "auto");
  return serialize(root, output, capacity);
}

bool buildListenStop(const char *sessionId, char *output, size_t capacity) {
  cJSON *root = baseMessage("listen", sessionId);
  if (!root) return false;
  cJSON_AddStringToObject(root, "state", "stop");
  return serialize(root, output, capacity);
}

bool buildAbort(const char *sessionId, char *output, size_t capacity) {
  cJSON *root = baseMessage("abort", sessionId);
  return root && serialize(root, output, capacity);
}

bool buildPing(char *output, size_t capacity) {
  cJSON *root = baseMessage("ping", nullptr);
  return root && serialize(root, output, capacity);
}

bool buildConversationReset(uint32_t requestId, char *output, size_t capacity) {
  if (!requestId) return false;
  cJSON *root = baseMessage("conversation", nullptr);
  if (!root) return false;
  cJSON_AddStringToObject(root, "action", "reset");
  cJSON_AddNumberToObject(root, "request_id", requestId);
  return serialize(root, output, capacity);
}

bool parseMessage(const uint8_t *json, size_t length, const char *expectedSessionId,
                  Message &message) {
  message.clear();
  if (!json || length == 0 || length > MaxJsonBytes) return false;
  const char *parseEnd = nullptr;
  const char *jsonText = reinterpret_cast<const char *>(json);
  cJSON *root = cJSON_ParseWithLengthOpts(jsonText, length, &parseEnd, false);
  while (root && parseEnd && parseEnd < jsonText + length &&
         isspace(static_cast<unsigned char>(*parseEnd))) ++parseEnd;
  if (root && (!parseEnd || parseEnd != jsonText + length)) {
    cJSON_Delete(root);
    root = nullptr;
  }
  if (!root || !cJSON_IsObject(root)) {
    if (root) cJSON_Delete(root);
    return false;
  }
  cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (!cJSON_IsString(type) || !type->valuestring) {
    cJSON_Delete(root);
    return false;
  }
  cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session_id");
  if (session && (!cJSON_IsString(session) || !session->valuestring ||
                  strlen(session->valuestring) > MaxSessionIdBytes)) {
    cJSON_Delete(root);
    return false;
  }
  if (session) copyBounded(session->valuestring, message.sessionId, sizeof(message.sessionId));
  if (expectedSessionId && expectedSessionId[0] && session &&
      strcmp(expectedSessionId, session->valuestring) != 0) {
    cJSON_Delete(root);
    return false;
  }

  if (!strcmp(type->valuestring, "hello")) {
    cJSON *transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
    if (!cJSON_IsString(transport) || strcmp(transport->valuestring, "websocket")) {
      cJSON_Delete(root);
      return false;
    }
    cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
    if (audio) {
      if (!cJSON_IsObject(audio)) {
        cJSON_Delete(root);
        return false;
      }
      cJSON *format = cJSON_GetObjectItemCaseSensitive(audio, "format");
      cJSON *rate = cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
      if ((format && (!cJSON_IsString(format) || strcmp(format->valuestring, "opus"))) ||
          !numberEquals(audio, "channels", 1) ||
          !numberEquals(audio, "frame_duration", 60) ||
          (rate && (!cJSON_IsNumber(rate) ||
                    (rate->valueint != 16000 && rate->valueint != 24000)))) {
        cJSON_Delete(root);
        return false;
      }
      if (rate) message.audio.sampleRate = static_cast<uint32_t>(rate->valueint);
    }
    message.type = MessageType::ServerHello;
  } else if (!strcmp(type->valuestring, "stt")) {
    cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (!cJSON_IsString(text)) { cJSON_Delete(root); return false; }
    copyUtf8Bounded(text->valuestring, message.text, sizeof(message.text));
    message.type = MessageType::Stt;
  } else if (!strcmp(type->valuestring, "tts")) {
    cJSON *state = cJSON_GetObjectItemCaseSensitive(root, "state");
    if (!cJSON_IsString(state)) { cJSON_Delete(root); return false; }
    if (!strcmp(state->valuestring, "start")) message.type = MessageType::TtsStart;
    else if (!strcmp(state->valuestring, "stop")) message.type = MessageType::TtsStop;
    else if (!strcmp(state->valuestring, "sentence_start")) {
      cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
      if (cJSON_IsString(text)) copyUtf8Bounded(text->valuestring, message.text, sizeof(message.text));
      message.type = MessageType::TtsCaption;
    } else message.type = MessageType::Ignored;
  } else if (!strcmp(type->valuestring, "llm")) {
    cJSON *emotion = cJSON_GetObjectItemCaseSensitive(root, "emotion");
    cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    if (cJSON_IsString(emotion)) message.emotion = parseEmotion(emotion->valuestring);
    if (cJSON_IsString(text)) copyUtf8Bounded(text->valuestring, message.text, sizeof(message.text));
    message.type = MessageType::Emotion;
  } else if (!strcmp(type->valuestring, "pong")) {
    message.type = MessageType::Pong;
  } else if (!strcmp(type->valuestring, "conversation")) {
    cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
    cJSON *requestId = cJSON_GetObjectItemCaseSensitive(root, "request_id");
    cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (!cJSON_IsString(action) || strcmp(action->valuestring, "reset") ||
        !cJSON_IsNumber(requestId) || requestId->valuedouble < 1 ||
        requestId->valuedouble > UINT32_MAX ||
        static_cast<double>(static_cast<uint32_t>(requestId->valuedouble)) !=
            requestId->valuedouble ||
        !cJSON_IsString(result) ||
        (strcmp(result->valuestring, "ok") && strcmp(result->valuestring, "error"))) {
      cJSON_Delete(root);
      return false;
    }
    message.requestId = static_cast<uint32_t>(requestId->valuedouble);
    message.resetSucceeded = !strcmp(result->valuestring, "ok");
    message.type = MessageType::ConversationReset;
  } else if (!strcmp(type->valuestring, "goodbye")) {
    message.type = MessageType::Goodbye;
  } else if (!strcmp(type->valuestring, "error")) {
    message.type = MessageType::Error;
    message.authenticationRejected = isAuthenticationError(root);
  } else if (!strcmp(type->valuestring, "mcp") || !strcmp(type->valuestring, "system") ||
             !strcmp(type->valuestring, "firmware") || !strcmp(type->valuestring, "mqtt") ||
             !strcmp(type->valuestring, "alert") || !strcmp(type->valuestring, "custom")) {
    message.type = MessageType::Ignored;
  } else {
    message.type = MessageType::Ignored;
  }
  cJSON_Delete(root);
  return true;
}

bool encodeAudioEnvelope(uint8_t version, const uint8_t *opus, size_t opusLength,
                         uint32_t timestamp, uint8_t *output, size_t capacity,
                         size_t &outputLength) {
  outputLength = 0;
  if (!opus || opusLength == 0 || opusLength > MaxAudioPacketBytes || !output) return false;
  if (version == 1) {
    if (capacity < opusLength) return false;
    memcpy(output, opus, opusLength);
    outputLength = opusLength;
    return true;
  }
  if (version == 2) {
    if (capacity < opusLength + 16U) return false;
    write16(output, 2); write16(output + 2, 0); write32(output + 4, 0);
    write32(output + 8, timestamp); write32(output + 12, static_cast<uint32_t>(opusLength));
    memcpy(output + 16, opus, opusLength);
    outputLength = opusLength + 16U;
    return true;
  }
  if (version == 3) {
    if (opusLength > UINT16_MAX || capacity < opusLength + 4U) return false;
    output[0] = 0; output[1] = 0; write16(output + 2, static_cast<uint16_t>(opusLength));
    memcpy(output + 4, opus, opusLength);
    outputLength = opusLength + 4U;
    return true;
  }
  return false;
}

bool decodeAudioEnvelope(uint8_t version, const uint8_t *input, size_t inputLength,
                         const uint8_t *&opus, size_t &opusLength,
                         uint32_t &timestamp) {
  opus = nullptr; opusLength = 0; timestamp = 0;
  if (!input || inputLength == 0) return false;
  if (version == 1) {
    if (inputLength > MaxAudioPacketBytes) return false;
    opus = input; opusLength = inputLength; return true;
  }
  if (version == 2) {
    if (inputLength < 16 || read16(input) != 2 || read16(input + 2) != 0 ||
        read32(input + 4) != 0) return false;
    const uint32_t length = read32(input + 12);
    if (length == 0 || length > MaxAudioPacketBytes || length != inputLength - 16U) return false;
    timestamp = read32(input + 8); opus = input + 16; opusLength = length; return true;
  }
  if (version == 3) {
    if (inputLength < 4 || input[0] != 0 || input[1] != 0) return false;
    const uint16_t length = read16(input + 2);
    if (length == 0 || length > MaxAudioPacketBytes || length != inputLength - 4U) return false;
    opus = input + 4; opusLength = length; return true;
  }
  return false;
}

}  // namespace XiaozhiProtocol
