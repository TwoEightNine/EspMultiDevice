#include <Arduino.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

namespace {
uint16_t passed = 0;
uint16_t failed = 0;

void check(bool condition, const char *name) {
  Serial.print(condition ? F("PASS ") : F("FAIL "));
  Serial.println(name);
  condition ? ++passed : ++failed;
}

void testUrls() {
  XiaozhiProtocol::ParsedUrl parsed = {};
  check(XiaozhiProtocol::parseHttpsUrl(
      "https://192-168-0-196.sslip.io:8443/xiaozhi/ota/", parsed), "https dns url");
  check(XiaozhiProtocol::parseWssUrl(
      "wss://192-168-0-196.sslip.io:8443/xiaozhi/v1/", parsed), "wss dns url");
  const char *invalid[] = {
    "http://host.example/a", "ws://host.example/a", "https://192.168.0.1/a",
    "https://[::1]/a", "https://user@host.example/a", "https://host.example/a#x",
    "https://host.example:0/a", "https://host.example:99999/a",
    "https://host.example:abc/a", "https:///a", "https://-bad.example/a"
  };
  for (const char *url : invalid) {
    check(!XiaozhiProtocol::parseHttpsUrl(url, parsed), "reject insecure/malformed url");
  }

  static constexpr char trust[] = "test-anchor";
  const AssistantBackendConfig valid = {
    "https://host.example/bootstrap", "https://host.example/activate",
    trust, sizeof(trust)
  };
  const AssistantBackendConfig emptyTrust = {
    valid.bootstrapUrl, valid.activationUrl, nullptr, 0
  };
  const AssistantBackendConfig plaintext = {
    "http://host.example/bootstrap", valid.activationUrl, trust, sizeof(trust)
  };
  check(XiaozhiBootstrap(valid).validConfiguration(), "accept injected secure backend");
  check(!XiaozhiBootstrap(emptyTrust).validConfiguration(), "reject empty trust anchor");
  check(!XiaozhiBootstrap(plaintext).validConfiguration(), "reject plaintext bootstrap");
}

void testJson() {
  char json[XiaozhiProtocol::MaxJsonBytes + 1] = {};
  check(XiaozhiProtocol::buildClientHello(3, json, sizeof(json)), "client hello");
  cJSON *root = cJSON_Parse(json);
  cJSON *features = root ? cJSON_GetObjectItemCaseSensitive(root, "features") : nullptr;
  cJSON *mcp = features ? cJSON_GetObjectItemCaseSensitive(features, "mcp") : nullptr;
  check(cJSON_IsFalse(mcp), "explicit features.mcp false");
  if (root) cJSON_Delete(root);

  memset(json, 0, sizeof(json));
  check(XiaozhiProtocol::buildPing(json, sizeof(json)) &&
        !strcmp(json, "{\"type\":\"ping\"}"), "heartbeat json");
  memset(json, 0, sizeof(json));
  check(XiaozhiProtocol::buildConversationReset(42, json, sizeof(json)) &&
        strstr(json, "\"type\":\"conversation\"") &&
        strstr(json, "\"action\":\"reset\"") &&
        strstr(json, "\"request_id\":42"), "conversation reset json");
  check(!XiaozhiProtocol::buildConversationReset(0, json, sizeof(json)),
        "reject zero reset request id");

  XiaozhiProtocol::Message message = {};
  const char hello[] = "{\"type\":\"hello\",\"transport\":\"websocket\",\"session_id\":\"s1\"}";
  check(XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(hello),
                                      strlen(hello), nullptr, message) &&
        message.audio.sampleRate == 16000 && message.audio.channels == 1 &&
        message.audio.frameDurationMs == 60, "omitted audio defaults");
  const char badAudio[] = "{\"type\":\"hello\",\"transport\":\"websocket\",\"audio_params\":{\"format\":\"pcm\",\"sample_rate\":8000,\"channels\":2,\"frame_duration\":20}}";
  check(!XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(badAudio),
                                       strlen(badAudio), nullptr, message),
        "reject incompatible audio");
  check(!XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(hello),
                                       strlen(hello), "other", message),
        "reject session mismatch");
  const char authError[] = "{\"type\":\"error\",\"code\":401}";
  check(XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(authError),
                                      strlen(authError), nullptr, message) &&
        message.type == XiaozhiProtocol::MessageType::Error &&
        message.authenticationRejected, "classify authentication error");
  const char malformed[] = "{not-json";
  check(!XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(malformed),
                                       strlen(malformed), nullptr, message), "reject malformed json");
  const char trailing[] = "{\"type\":\"goodbye\"}garbage";
  check(!XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(trailing),
                                       strlen(trailing), nullptr, message),
        "reject trailing json data");
  uint8_t oversized[XiaozhiProtocol::MaxJsonBytes + 1] = {};
  check(!XiaozhiProtocol::parseMessage(oversized, sizeof(oversized), nullptr, message),
        "reject oversized json");

  char captionJson[512] = {};
  memset(captionJson, 'a', sizeof(captionJson));
  const char prefix[] = "{\"type\":\"stt\",\"text\":\"";
  memcpy(captionJson, prefix, strlen(prefix));
  captionJson[sizeof(captionJson) - 3] = '"';
  captionJson[sizeof(captionJson) - 2] = '}';
  captionJson[sizeof(captionJson) - 1] = 0;
  check(XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(captionJson),
                                      strlen(captionJson), nullptr, message) &&
        strlen(message.text) == AssistantSnapshot::CaptionBytes,
        "truncate caption");

  const char chineseStt[] = "{\"type\":\"stt\",\"text\":\"\\u4f60\\u597d\\uff0c\\u4e16\\u754c\"}";
  check(XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(chineseStt),
                                      strlen(chineseStt), nullptr, message) &&
        !strcmp(message.text, "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C"),
        "parse Simplified-Chinese caption");

  const char pong[] = "{\"type\":\"pong\",\"timestamp\":1}";
  check(XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(pong),
                                      strlen(pong), nullptr, message) &&
        message.type == XiaozhiProtocol::MessageType::Pong, "parse heartbeat pong");
  const char resetOk[] = "{\"type\":\"conversation\",\"action\":\"reset\",\"request_id\":42,\"result\":\"ok\"}";
  check(XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(resetOk),
                                      strlen(resetOk), nullptr, message) &&
        message.type == XiaozhiProtocol::MessageType::ConversationReset &&
        message.requestId == 42 && message.resetSucceeded, "parse reset acknowledgement");
  const char resetError[] = "{\"type\":\"conversation\",\"action\":\"reset\",\"request_id\":42,\"result\":\"error\",\"category\":\"storage\"}";
  check(XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(resetError),
                                      strlen(resetError), nullptr, message) &&
        message.requestId == 42 && !message.resetSucceeded, "parse reset rejection");
  const char *badReset[] = {
    "{\"type\":\"conversation\",\"action\":\"drop\",\"request_id\":42,\"result\":\"ok\"}",
    "{\"type\":\"conversation\",\"action\":\"reset\",\"request_id\":0,\"result\":\"ok\"}",
    "{\"type\":\"conversation\",\"action\":\"reset\",\"request_id\":1.5,\"result\":\"ok\"}",
    "{\"type\":\"conversation\",\"action\":\"reset\",\"request_id\":42,\"result\":\"maybe\"}"
  };
  for (const char *value : badReset) {
    check(!XiaozhiProtocol::parseMessage(reinterpret_cast<const uint8_t *>(value),
                                         strlen(value), nullptr, message),
          "reject malformed reset acknowledgement");
  }

  const char fourChinese[] =
      "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C";
  char truncated[10] = {};
  check(!XiaozhiProtocol::copyUtf8Bounded(fourChinese, truncated, sizeof(truncated)) &&
        !strcmp(truncated, "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96"),
        "truncate Chinese caption at UTF-8 boundary");

  const char malformedUtf8[] = {'\xE4', 'A', 0};
  char repaired[8] = {};
  check(XiaozhiProtocol::copyUtf8Bounded(malformedUtf8, repaired, sizeof(repaired)) &&
        !strcmp(repaired, "?A"), "replace malformed UTF-8");

  const char overlongUtf8[] = {'\xE0', '\x80', '\x80', 0};
  memset(repaired, 0, sizeof(repaired));
  check(XiaozhiProtocol::copyUtf8Bounded(overlongUtf8, repaired, sizeof(repaired)) &&
        !strcmp(repaired, "???"), "replace overlong UTF-8");
}

void testAudio() {
  uint8_t source[64] = {};
  for (size_t i = 0; i < sizeof(source); ++i) source[i] = static_cast<uint8_t>(i + 1);
  for (uint8_t version = 1; version <= 3; ++version) {
    uint8_t envelope[96] = {};
    size_t envelopeLength = 0;
    const uint8_t *decoded = nullptr;
    size_t decodedLength = 0;
    uint32_t timestamp = 0;
    check(XiaozhiProtocol::encodeAudioEnvelope(version, source, sizeof(source), 1234,
                                                envelope, sizeof(envelope), envelopeLength) &&
          XiaozhiProtocol::decodeAudioEnvelope(version, envelope, envelopeLength,
                                                decoded, decodedLength, timestamp) &&
          decodedLength == sizeof(source) && !memcmp(decoded, source, sizeof(source)) &&
          (version == 1 || timestamp == 1234), "v1/v2/v3 audio envelope");
    if (version > 1) {
      check(!XiaozhiProtocol::decodeAudioEnvelope(version, envelope, envelopeLength - 1,
                                                   decoded, decodedLength, timestamp),
            "reject malformed envelope length");
    }
  }
}

void testStateAndQueues() {
  check(assistantTransitionAllowed(AssistantPhase::NeedsWiFi, AssistantPhase::Bootstrapping) &&
        assistantTransitionAllowed(AssistantPhase::Bootstrapping, AssistantPhase::Connecting) &&
        assistantTransitionAllowed(AssistantPhase::Connecting, AssistantPhase::Ready) &&
        assistantTransitionAllowed(AssistantPhase::Ready, AssistantPhase::Listening) &&
        assistantTransitionAllowed(AssistantPhase::Listening, AssistantPhase::Thinking) &&
        assistantTransitionAllowed(AssistantPhase::Thinking, AssistantPhase::Speaking) &&
        assistantTransitionAllowed(AssistantPhase::Speaking, AssistantPhase::Ready) &&
        assistantTransitionAllowed(AssistantPhase::Error, AssistantPhase::Bootstrapping),
        "state transitions");
  check(!assistantGenerationMatches(8, 7) && assistantGenerationMatches(8, 8),
        "generation cancellation");
  check(assistantErrorName(AssistantError::TimeoutBootstrap)[0] &&
        assistantErrorName(AssistantError::TimeoutConnect)[0] &&
        assistantErrorName(AssistantError::TimeoutHello)[0] &&
        assistantErrorName(AssistantError::TimeoutListening)[0] &&
        assistantErrorName(AssistantError::TimeoutThinking)[0] &&
        assistantErrorName(AssistantError::TimeoutSpeakingPacket)[0] &&
        assistantErrorName(AssistantError::TimeoutSpeakingTotal)[0] &&
        assistantErrorName(AssistantError::TimeoutTurn)[0] &&
        assistantErrorName(AssistantError::TimeoutConversationReset)[0],
        "timeout categories");
  check(XiaozhiSession::CommandQueueCapacity == 4 &&
        XiaozhiSession::EventQueueCapacity == 8 &&
        XiaozhiSession::UplinkQueueCapacity == 4 &&
        XiaozhiSession::DownlinkQueueCapacity == 8, "bounded queue contracts");
  QueueHandle_t queue = xQueueCreate(XiaozhiSession::CommandQueueCapacity, sizeof(uint32_t));
  uint32_t value = 1;
  bool bounded = queue;
  for (size_t i = 0; bounded && i < XiaozhiSession::CommandQueueCapacity; ++i) {
    bounded = xQueueSend(queue, &value, 0) == pdTRUE;
  }
  bounded = bounded && xQueueSend(queue, &value, 0) == pdFALSE;
  check(bounded, "queue overflow rejects producer");
  if (queue) vQueueDelete(queue);
}

void testTranscript() {
  AssistantTranscript transcript;
  check(transcript.begin(), "allocate transcript in PSRAM");
  transcript.beginDraft();
  transcript.setUser("cancel me");
  transcript.appendAssistant("incomplete");
  transcript.discardDraft();
  check(transcript.count() == 0, "discard cancelled transcript draft");

  for (uint8_t index = 0; index < 6; ++index) {
    char user[24] = {};
    snprintf(user, sizeof(user), "user-%u", index);
    transcript.beginDraft();
    transcript.setUser(user);
    transcript.appendAssistant(index == 5 ? "first sentence" : "answer");
    if (index == 5) transcript.appendAssistant("second sentence");
    check(transcript.completeDraft(), "complete transcript exchange");
  }
  AssistantTranscriptTurn turn = {};
  check(transcript.count() == AssistantTranscript::Capacity &&
        transcript.copyTurn(0, turn) && !strcmp(turn.user, "user-1"),
        "sixth exchange evicts oldest transcript turn");
  check(transcript.copyTurn(4, turn) &&
        !strcmp(turn.assistant, "first sentence second sentence"),
        "append multiple assistant sentences");

  char longChinese[AssistantTranscriptTurn::UserBytes + 8] = {};
  size_t used = 0;
  while (used + 3 < sizeof(longChinese)) {
    memcpy(longChinese + used, "\xE4\xBD\xA0", 3);
    used += 3;
  }
  transcript.beginDraft();
  transcript.setUser(longChinese);
  transcript.appendAssistant("ok");
  check(transcript.copyDraft(turn) && strlen(turn.user) <=
        AssistantTranscriptTurn::UserBytes &&
        (strlen(turn.user) % 3) == 0, "UTF-8-safe transcript bound");
  transcript.end();
}

void testAvatarAssets() {
  check(AssistantAvatar::assetFor(AssistantPhase::Bootstrapping,
                                  AssistantEmotion::Neutral) == AssistantAvatar::Asset::Waiting &&
        AssistantAvatar::assetFor(AssistantPhase::Error,
                                  AssistantEmotion::Happy) == AssistantAvatar::Asset::Waiting,
        "waiting asset for setup and error");
  check(AssistantAvatar::assetFor(AssistantPhase::Speaking,
                                  AssistantEmotion::Sad) == AssistantAvatar::Asset::Speaking,
        "speaking asset precedence");
  check(AssistantAvatar::assetFor(AssistantPhase::Ready,
                                  AssistantEmotion::Happy) == AssistantAvatar::Asset::Happy &&
        AssistantAvatar::assetFor(AssistantPhase::Ready,
                                  AssistantEmotion::Sad) == AssistantAvatar::Asset::Sad &&
        AssistantAvatar::assetFor(AssistantPhase::Ready,
                                  AssistantEmotion::Neutral) == AssistantAvatar::Asset::Neutral,
        "emotion asset mapping");
  check(!strcmp(AssistantAvatar::filename(AssistantAvatar::Asset::Waiting), "w.png") &&
        !strcmp(AssistantAvatar::filename(AssistantAvatar::Asset::Neutral), "n.png") &&
        !strcmp(AssistantAvatar::filename(AssistantAvatar::Asset::Sad), "s.png") &&
        !strcmp(AssistantAvatar::filename(AssistantAvatar::Asset::Happy), "h.png") &&
        !strcmp(AssistantAvatar::filename(AssistantAvatar::Asset::Speaking), "t.png"),
        "avatar asset filenames");

  char name[AssistantAvatarLoader::AvatarNameBytes] = {};
  char directory[AssistantAvatarLoader::AvatarDirectoryBytes] = {};
  check(AssistantAvatarLoader::parseConfigLine("  Friendly avatar : friendly_01  \r",
                                              name, sizeof(name), directory,
                                              sizeof(directory)) &&
        !strcmp(name, "Friendly avatar") && !strcmp(directory, "friendly_01"),
        "parse avatar config entry");
  check(!AssistantAvatarLoader::parseConfigLine("Missing directory:", name, sizeof(name),
                                                directory, sizeof(directory)) &&
        !AssistantAvatarLoader::parseConfigLine("Broken:../outside", name, sizeof(name),
                                                directory, sizeof(directory)) &&
        !AssistantAvatarLoader::parseConfigLine("Too:many:separators", name, sizeof(name),
                                                directory, sizeof(directory)),
        "reject malformed avatar config entry");
  char path[128] = {};
  check(AssistantAvatarLoader::buildAvatarPath("friendly_01",
                                               AssistantAvatar::Asset::Happy,
                                               path, sizeof(path)) &&
        !strcmp(path, "/.system/ai_avatars/friendly_01/h.png") &&
        !AssistantAvatarLoader::buildAvatarPath("../outside",
                                                AssistantAvatar::Asset::Happy,
                                                path, sizeof(path)),
        "build selected avatar path");
}
}  // namespace

void assistantContractSetup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("ESP Media Stage 2 assistant contract"));
  testUrls();
  testJson();
  testAudio();
  testStateAndQueues();
  testTranscript();
  testAvatarAssets();
  Serial.print(F("ASSISTANT_CONTRACT "));
  Serial.print(failed == 0 ? F("PASS ") : F("FAIL "));
  Serial.print(passed);
  Serial.print('/');
  Serial.println(passed + failed);
}

void assistantContractLoop() { delay(1000); }
