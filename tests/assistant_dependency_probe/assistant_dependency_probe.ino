#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <new>
#include <time.h>

#if __has_include("ProbeConfig.h")
#include "ProbeConfig.h"
#define ESP_MEDIA_PROBE_HAS_WSS_CONFIG 1
#else
#define ESP_MEDIA_PROBE_HAS_WSS_CONFIG 0
namespace AssistantProbeConfig {
inline constexpr char Host[] = "not-configured.invalid";
inline constexpr uint16_t Port = 443;
inline constexpr char Path[] = "/echo";
inline constexpr char Subprotocol[] = "esp-media-stage0";
inline constexpr char HeaderName[] = "X-ESP-Media-Probe";
inline constexpr char HeaderValue[] = "stage0";
inline constexpr char HeaderAcceptedMessage[] = "header-ok";
inline constexpr char CorrectCaPem[] = "";
inline constexpr char WrongCaPem[] = "";
} // namespace AssistantProbeConfig
#endif

namespace {

constexpr size_t CodecFrames = 1000;
constexpr size_t EchoRounds = 1000;
constexpr size_t ConnectCycles = 20;
constexpr uint32_t ConnectTimeoutMs = 15000;
constexpr uint32_t EchoTimeoutMs = 5000;
constexpr uint32_t TimeSyncTimeoutMs = 30000;
constexpr size_t EchoBinaryBytes = 32;
constexpr time_t MinimumValidEpoch = 1609459200; // 2021-01-01 UTC
// Fixed-point Opus has substantial temporary call-stack use. Keep the probe
// out of Arduino's small loopTask stack; production firmware never starts
// this task because the complete probe is compile-time gated.
constexpr uint32_t ProbeTaskStackBytes = 48U * 1024U;

struct MemorySample {
  size_t freeInternal;
  size_t largestInternal;
  size_t freePsram;
};

SecureWebSocketClient *websocket = nullptr;
SecureWebSocketClient::Event websocketEvent;
MemorySample stage0Baseline = {};
volatile bool runConfiguredWss = ESP_MEDIA_PROBE_HAS_WSS_CONFIG != 0;

MemorySample memorySample() {
  MemorySample sample;
  sample.freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  sample.largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  sample.freePsram = ESP.getFreePsram();
  return sample;
}

void printMemory(const char *label, const MemorySample &sample) {
  Serial.print(label);
  Serial.print(F(" free_internal="));
  Serial.print(sample.freeInternal);
  Serial.print(F(" largest_internal="));
  Serial.print(sample.largestInternal);
  Serial.print(F(" free_psram="));
  Serial.println(sample.freePsram);
}

int compareUint32(const void *left, const void *right) {
  uint32_t a = *static_cast<const uint32_t *>(left);
  uint32_t b = *static_cast<const uint32_t *>(right);
  return a < b ? -1 : (a > b ? 1 : 0);
}

uint32_t percentile95(uint32_t *values, size_t count) {
  qsort(values, count, sizeof(values[0]), compareUint32);
  return values[((count * 95U) + 99U) / 100U - 1U];
}

void fillPcm(int16_t *pcm, uint32_t &state) {
  for (size_t i = 0; i < OpusCodec::EncoderFrameSamples; ++i) {
    state = state * 1664525UL + 1013904223UL;
    pcm[i] = static_cast<int16_t>((static_cast<int32_t>(state >> 16) % 12001) - 6000);
  }
}

bool runCodecRate(uint32_t decoderRate) {
  int16_t *input = static_cast<int16_t *>(heap_caps_malloc(
    OpusCodec::EncoderFrameSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  int16_t *output = static_cast<int16_t *>(heap_caps_malloc(
    1440 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint8_t *packet = static_cast<uint8_t *>(heap_caps_malloc(
    OpusCodec::MaxPacketBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint32_t *encodeTimes = static_cast<uint32_t *>(heap_caps_malloc(
    CodecFrames * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  uint32_t *decodeTimes = static_cast<uint32_t *>(heap_caps_malloc(
    CodecFrames * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

  if (!input || !output || !packet || !encodeTimes || !decodeTimes) {
    Serial.println(F("FAIL codec probe: PSRAM buffer allocation"));
    free(decodeTimes);
    free(encodeTimes);
    free(packet);
    free(output);
    free(input);
    return false;
  }

  MemorySample before = memorySample();
  OpusCodec codec;
  bool ok = codec.begin(decoderRate);
  MemorySample initialized = memorySample();
  if (!ok) {
    Serial.print(F("FAIL codec init: "));
    Serial.println(codec.lastError());
  }

  size_t expectedSamples = decoderRate == 16000 ? 960 : 1440;
  size_t maxPacket = 0;
  size_t previousCheckpointHeap = initialized.freeInternal;
  size_t consecutiveHeapDrops = 0;
  uint32_t pcmState = 0x6d2b79f5UL;
  for (size_t frame = 0; ok && frame < CodecFrames; ++frame) {
    fillPcm(input, pcmState);
    uint32_t started = micros();
    int encoded = codec.encode60Ms(input,
                                   OpusCodec::EncoderFrameSamples,
                                   packet,
                                   OpusCodec::MaxPacketBytes);
    encodeTimes[frame] = micros() - started;
    if (encoded <= 0 || encoded > static_cast<int>(OpusCodec::MaxPacketBytes)) {
      Serial.print(F("FAIL encode frame="));
      Serial.print(frame);
      Serial.print(F(" result="));
      Serial.println(encoded);
      ok = false;
      break;
    }
    if (static_cast<size_t>(encoded) > maxPacket) maxPacket = encoded;

    started = micros();
    int decoded = codec.decode(packet,
                               static_cast<size_t>(encoded),
                               output,
                               expectedSamples);
    decodeTimes[frame] = micros() - started;
    if (decoded != static_cast<int>(expectedSamples)) {
      Serial.print(F("FAIL decode rate="));
      Serial.print(decoderRate);
      Serial.print(F(" frame="));
      Serial.print(frame);
      Serial.print(F(" samples="));
      Serial.println(decoded);
      ok = false;
      break;
    }
    if ((frame + 1) % 100 == 0) {
      size_t checkpointHeap = memorySample().freeInternal;
      if (checkpointHeap + 128 < previousCheckpointHeap) {
        ++consecutiveHeapDrops;
      } else {
        consecutiveHeapDrops = 0;
      }
      previousCheckpointHeap = checkpointHeap;
      if (consecutiveHeapDrops >= 3) {
        Serial.println(F("FAIL codec: monotonic internal heap loss"));
        ok = false;
        break;
      }
    }
    delay(0);
  }

  uint32_t encodeP95 = ok ? percentile95(encodeTimes, CodecFrames) : 0;
  uint32_t decodeP95 = ok ? percentile95(decodeTimes, CodecFrames) : 0;
  size_t internalGrowth = before.freeInternal > initialized.freeInternal
    ? before.freeInternal - initialized.freeInternal : 0;
  size_t combinedInternalGrowth = stage0Baseline.freeInternal > initialized.freeInternal
    ? stage0Baseline.freeInternal - initialized.freeInternal : 0;

  Serial.print(F("CODEC rate="));
  Serial.print(decoderRate);
  Serial.print(F(" frames="));
  Serial.print(CodecFrames);
  Serial.print(F(" decode_samples="));
  Serial.print(expectedSamples);
  Serial.print(F(" max_packet="));
  Serial.print(maxPacket);
  Serial.print(F(" encode_p95_us="));
  Serial.print(encodeP95);
  Serial.print(F(" decode_p95_us="));
  Serial.print(decodeP95);
  Serial.print(F(" init_internal_growth="));
  Serial.print(internalGrowth);
  Serial.print(F(" combined_internal_growth="));
  Serial.println(combinedInternalGrowth);

  if (internalGrowth > 32U * 1024U || combinedInternalGrowth > 32U * 1024U) {
    Serial.println(F("FAIL transport/codec: permanent internal growth exceeds 32 KiB"));
    ok = false;
  }
  if (encodeP95 >= 30000U || decodeP95 >= 20000U) {
    Serial.println(F("FAIL codec: timing gate"));
    ok = false;
  }

  MemorySample active = memorySample();
  printMemory("CODEC_ACTIVE", active);
  if (active.freeInternal + 256 < initialized.freeInternal ||
      active.freePsram + 256 < initialized.freePsram) {
    Serial.println(F("FAIL codec: heap loss across frames"));
    ok = false;
  }
  if (active.largestInternal < 96U * 1024U) {
    Serial.println(F("FAIL codec: active largest internal block below 96 KiB"));
    ok = false;
  }

  codec.end();
  MemorySample ended = memorySample();
  if (ended.freeInternal + 256 < before.freeInternal) {
    Serial.println(F("FAIL codec: internal heap not recovered"));
    ok = false;
  }
  if (ended.largestInternal < 96U * 1024U) {
    Serial.println(F("FAIL codec: largest internal block below 96 KiB"));
    ok = false;
  }
  printMemory("CODEC_END", ended);

  free(decodeTimes);
  free(encodeTimes);
  free(packet);
  free(output);
  free(input);
  return ok;
}

bool waitForConnection(uint32_t timeoutMs) {
  uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (websocket->poll(websocketEvent) &&
        websocketEvent.type == SecureWebSocketClient::EventType::Connected) {
      return true;
    }
    delay(1);
  }
  return false;
}

bool waitForPayload(SecureWebSocketClient::EventType wanted,
                    const uint8_t *expected,
                    size_t expectedLength,
                    uint32_t timeoutMs) {
  uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (!websocket->poll(websocketEvent)) {
      delay(1);
      continue;
    }
    if (websocketEvent.type == SecureWebSocketClient::EventType::Error ||
        websocketEvent.type == SecureWebSocketClient::EventType::Disconnected ||
        websocketEvent.type == SecureWebSocketClient::EventType::Overflow) {
      return false;
    }
    if (websocketEvent.type == wanted &&
        websocketEvent.length == expectedLength &&
        memcmp(websocketEvent.payload, expected, expectedLength) == 0) {
      return true;
    }
  }
  return false;
}

bool verifyMissingTrustRejected() {
  const SecureWebSocketClient::Endpoint endpoint = {"invalid.invalid", 443, "/", ""};
  const SecureWebSocketClient::TrustStore missing = {
    SecureWebSocketClient::TrustKind::PemCertificate, nullptr, 0
  };
  bool rejected = !websocket->connect(endpoint, nullptr, 0, missing);
  websocket->close();
  Serial.println(rejected ? F("PASS WSS missing CA rejected")
                          : F("FAIL WSS missing CA accepted"));
  return rejected;
}

bool verifyPlatformDependencies() {
  HTTPClient http;
  Preferences preferences;
  cJSON *document = cJSON_Parse("{\"stage\":0,\"ok\":true}");
  cJSON *stage = document ? cJSON_GetObjectItemCaseSensitive(document, "stage") : nullptr;
  bool ok = cJSON_IsNumber(stage) && stage->valueint == 0;
  cJSON_Delete(document);
  (void)http;
  (void)preferences;
  Serial.println(ok ? F("PASS Arduino-ESP32 HTTP/TLS/Preferences/cJSON availability")
                    : F("FAIL Arduino-ESP32 cJSON probe"));
  return ok;
}

SecureWebSocketClient::Endpoint configuredEndpoint() {
  return {AssistantProbeConfig::Host,
          AssistantProbeConfig::Port,
          AssistantProbeConfig::Path,
          AssistantProbeConfig::Subprotocol};
}

SecureWebSocketClient::Header configuredHeader() {
  return {AssistantProbeConfig::HeaderName, AssistantProbeConfig::HeaderValue};
}

SecureWebSocketClient::TrustStore pemTrust(const char *pem, size_t length) {
  return {SecureWebSocketClient::TrustKind::PemCertificate,
          reinterpret_cast<const uint8_t *>(pem), length};
}

// WebSocketsClient reduces a failed TLS setup to a generic disconnect event.
// Keep this diagnostic-only direct connection so a fixture or certificate
// problem is reported with NetworkClientSecure's actual mbedTLS error.
bool verifyDirectTlsWithCorrectTrust() {
  NetworkClientSecure tls;
  tls.setCACert(AssistantProbeConfig::CorrectCaPem);
  tls.setHandshakeTimeout(ConnectTimeoutMs / 1000U);
  bool connected = tls.connect(AssistantProbeConfig::Host,
                               AssistantProbeConfig::Port,
                               static_cast<int32_t>(ConnectTimeoutMs));
  if (!connected) {
    char detail[192] = {};
    int error = tls.lastError(detail, sizeof(detail));
    Serial.print(F("FAIL TLS valid CA direct connect error="));
    Serial.print(error);
    Serial.print(F(" detail="));
    Serial.println(detail);
    tls.stop();
    return false;
  }
  tls.stop();
  Serial.println(F("PASS TLS valid CA direct connect"));
  return true;
}

bool verifyWrongTrustRejected() {
  SecureWebSocketClient::Endpoint endpoint = configuredEndpoint();
  SecureWebSocketClient::Header header = configuredHeader();
  SecureWebSocketClient::TrustStore wrong = pemTrust(
    AssistantProbeConfig::WrongCaPem, sizeof(AssistantProbeConfig::WrongCaPem));
  // The direct secure adapter performs the TLS handshake inside connect(), so
  // a bad CA is rejected synchronously. Keep that intentional failure on a
  // short-lived instance: mbedTLS certificate parsing allocates internal heap
  // and must not distort the subsequent valid-session measurement.
  SecureWebSocketClient rejectedClient;
  bool rejected = !rejectedClient.connect(endpoint, &header, 1, wrong);
  rejectedClient.close();
  Serial.println(rejected ? F("PASS WSS wrong CA rejected")
                           : F("FAIL WSS wrong CA connected"));
  return rejected;
}

bool connectWithCorrectTrust() {
  SecureWebSocketClient::Endpoint endpoint = configuredEndpoint();
  SecureWebSocketClient::Header header = configuredHeader();
  SecureWebSocketClient::TrustStore correct = pemTrust(
    AssistantProbeConfig::CorrectCaPem, sizeof(AssistantProbeConfig::CorrectCaPem));
  if (!websocket->connect(endpoint, &header, 1, correct)) {
    Serial.print(F("FAIL WSS valid CA did not start: "));
    Serial.println(websocket->lastError());
    return false;
  }
  if (!waitForConnection(ConnectTimeoutMs)) {
    Serial.print(F("FAIL WSS valid CA did not connect: "));
    Serial.println(websocket->lastError());
    return false;
  }
  if (!waitForPayload(SecureWebSocketClient::EventType::Text,
                      reinterpret_cast<const uint8_t *>(AssistantProbeConfig::HeaderAcceptedMessage),
                      strlen(AssistantProbeConfig::HeaderAcceptedMessage),
                      EchoTimeoutMs)) {
    Serial.print(F("FAIL WSS custom-header acknowledgement: "));
    Serial.println(websocket->lastError());
    return false;
  }
  return true;
}

bool waitForNetworkTime() {
  // TLS certificate validity depends on a sane clock. The normal firmware
  // obtains this through TimeController; the isolated diagnostic starts SNTP
  // directly after its saved Wi-Fi connection succeeds.
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  uint32_t started = millis();
  while (millis() - started < TimeSyncTimeoutMs) {
    time_t now = time(nullptr);
    if (now >= MinimumValidEpoch) {
      Serial.print(F("PASS WSS time synced epoch="));
      Serial.println(static_cast<unsigned long>(now));
      return true;
    }
    delay(100);
  }
  Serial.println(F("FAIL WSS: SNTP time unavailable; TLS certificate validation cannot run"));
  return false;
}

bool runEchoRounds() {
  char textPayload[24];
  uint8_t binaryPayload[EchoBinaryBytes];

  for (size_t round = 0; round < EchoRounds; ++round) {
    int textLength = snprintf(textPayload, sizeof(textPayload), "text:%04u", static_cast<unsigned>(round));
    if (textLength <= 0) {
      Serial.println(F("FAIL WSS text payload formatting"));
      return false;
    }
    if (!websocket->sendText(textPayload, static_cast<size_t>(textLength))) {
      Serial.print(F("FAIL WSS text send round="));
      Serial.println(round);
      Serial.println(websocket->lastError());
      return false;
    }
    if (!waitForPayload(SecureWebSocketClient::EventType::Text,
                        reinterpret_cast<const uint8_t *>(textPayload),
                        static_cast<size_t>(textLength), EchoTimeoutMs)) {
      Serial.print(F("FAIL WSS text echo round="));
      Serial.println(round);
      Serial.println(websocket->lastError());
      return false;
    }

    for (size_t i = 0; i < sizeof(binaryPayload); ++i) {
      binaryPayload[i] = static_cast<uint8_t>((round * 31U + i * 17U) & 0xffU);
    }
    if (!websocket->sendBinary(binaryPayload, sizeof(binaryPayload))) {
      Serial.print(F("FAIL WSS binary send round="));
      Serial.println(round);
      Serial.println(websocket->lastError());
      return false;
    }
    if (!waitForPayload(SecureWebSocketClient::EventType::Binary,
                        binaryPayload, sizeof(binaryPayload), EchoTimeoutMs)) {
      Serial.print(F("FAIL WSS binary echo round="));
      Serial.println(round);
      Serial.println(websocket->lastError());
      return false;
    }
    delay(0);
  }
  Serial.println(F("PASS WSS 1000 text + 1000 binary round trips"));
  return true;
}

bool runConfiguredWssProbe() {
  // Match production WiFiController behavior. This has to happen before the
  // first Wi-Fi initialization; it trades fixed driver buffers for a stable
  // internal heap suitable for TLS and the Stage 0 largest-block gate.
  WiFi.useStaticBuffers(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  uint32_t wifiStarted = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStarted < 30000U) {
    delay(50);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("FAIL WSS: saved Wi-Fi did not connect"));
    return false;
  }
  printMemory("WIFI_READY", memorySample());
  if (!waitForNetworkTime()) {
    return false;
  }

  bool ok = verifyWrongTrustRejected();
  if (!ok || !connectWithCorrectTrust()) {
    Serial.println(F("FAIL WSS valid CA or custom-header acknowledgement"));
    websocket->close();
    return false;
  }
  Serial.println(F("PASS WSS valid CA and custom header"));

  ok = runEchoRounds();
  MemorySample active = memorySample();
  printMemory("WSS_ACTIVE", active);
  if (active.largestInternal < 96U * 1024U) {
    Serial.println(F("FAIL WSS active largest internal block below 96 KiB"));
    ok = false;
  }
  websocket->close();
  delay(100);
  MemorySample firstClosed = memorySample();
  size_t lowestClosedHeap = firstClosed.freeInternal;

  for (size_t cycle = 1; ok && cycle < ConnectCycles; ++cycle) {
    if (!connectWithCorrectTrust()) {
      Serial.print(F("FAIL WSS reconnect cycle="));
      Serial.println(cycle + 1);
      ok = false;
      break;
    }
    websocket->close();
    delay(100);
    size_t freeHeap = memorySample().freeInternal;
    if (freeHeap < lowestClosedHeap) lowestClosedHeap = freeHeap;
    if (freeHeap + 1024 < firstClosed.freeInternal) {
      Serial.print(F("FAIL WSS reconnect heap loss cycle="));
      Serial.println(cycle + 1);
      ok = false;
    }
  }

  MemorySample ended = memorySample();
  printMemory("WSS_END", ended);
  if (ended.largestInternal < 96U * 1024U) {
    Serial.println(F("FAIL WSS largest internal block below 96 KiB"));
    ok = false;
  }
  if (ok) Serial.println(F("PASS WSS 20 connect/disconnect cycles"));
  return ok;
}

} // namespace

void runAssistantDependencyProbe() {
  Serial.println(F("ESP Media Stage 0 assistant dependency probe"));
  Serial.print(F("CPU MHz="));
  Serial.println(getCpuFrequencyMhz());
  Serial.print(F("probe_task_stack_bytes="));
  Serial.println(ProbeTaskStackBytes);
  stage0Baseline = memorySample();
  printMemory("START", stage0Baseline);

  websocket = new (std::nothrow) SecureWebSocketClient();
  MemorySample transportReady = memorySample();
  size_t transportGrowth = stage0Baseline.freeInternal > transportReady.freeInternal
    ? stage0Baseline.freeInternal - transportReady.freeInternal : 0;
  Serial.print(F("TRANSPORT init_internal_growth="));
  Serial.println(transportGrowth);

  bool ok = verifyPlatformDependencies();
  if (!websocket || transportGrowth > 32U * 1024U) {
    Serial.println(F("FAIL transport initialization/internal-growth gate"));
    ok = false;
  } else {
    ok = verifyMissingTrustRejected() && ok;
  }
  ok = runCodecRate(16000) && ok;
  ok = runCodecRate(24000) && ok;

  if (runConfiguredWss) {
    if (websocket) ok = runConfiguredWssProbe() && ok;
  } else {
    Serial.println(F("SKIP WSS network probe: copy ProbeConfig.example.h to ProbeConfig.h and configure it"));
  }

  delete websocket;
  websocket = nullptr;
  printMemory("FINAL", memorySample());
  Serial.print(F("probe_task_stack_high_water_bytes="));
  Serial.println(uxTaskGetStackHighWaterMark(nullptr));
  Serial.println(ok ? F("STAGE0_PROBE PASS") : F("STAGE0_PROBE FAIL"));
}

void assistantDependencyProbeTask(void *) {
  runAssistantDependencyProbe();
  vTaskDelete(nullptr);
}

void assistantDependencyProbeSetup() {
  Serial.begin(115200);
  uint32_t serialStarted = millis();
  while (!Serial && millis() - serialStarted < 2000U) {
    delay(10);
  }
  BaseType_t created = xTaskCreatePinnedToCore(assistantDependencyProbeTask,
                                                "assistantProbe",
                                                ProbeTaskStackBytes,
                                                nullptr,
                                                tskIDLE_PRIORITY + 1,
                                                nullptr,
                                                xPortGetCoreID());
  if (created != pdPASS) {
    Serial.println(F("FAIL probe task creation"));
  }
}

void assistantDependencyProbeLoop() {
  delay(1000);
}
