#include <Arduino.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

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
        assistantTransitionAllowed(AssistantPhase::Bootstrapping, AssistantPhase::Ready) &&
        assistantTransitionAllowed(AssistantPhase::Ready, AssistantPhase::Connecting) &&
        assistantTransitionAllowed(AssistantPhase::Connecting, AssistantPhase::Listening) &&
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
        assistantErrorName(AssistantError::TimeoutTurn)[0], "timeout categories");
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
}  // namespace

void assistantContractSetup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("ESP Media Stage 2 assistant contract"));
  testUrls();
  testJson();
  testAudio();
  testStateAndQueues();
  Serial.print(F("ASSISTANT_CONTRACT "));
  Serial.print(failed == 0 ? F("PASS ") : F("FAIL "));
  Serial.print(passed);
  Serial.print('/');
  Serial.println(passed + failed);
}

void assistantContractLoop() { delay(1000); }
