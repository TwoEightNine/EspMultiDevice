#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <time.h>

#include "../../src/services/assistant/SecureWebSocketClient.h"
#include "../../src/services/assistant/XiaozhiBootstrapClient.h"
#include "../../src/services/assistant/XiaozhiIdentity.h"
#include "../../src/services/assistant/XiaozhiTrust.h"

#if __has_include("LocalBackendConfig.h")
#include "LocalBackendConfig.h"
#else
namespace XiaozhiProbeConfig {
inline constexpr char TargetName[] = "official-hosted";
inline constexpr const char *BootstrapUrl = XiaozhiBootstrapClient::OfficialBootstrapUrl;
inline constexpr const char *ActivationUrl = XiaozhiBootstrapClient::OfficialActivationUrl;
inline constexpr const char *CaCertificate = XiaozhiTrust::DigiCertGlobalRootG2;
} // namespace XiaozhiProbeConfig
#endif

namespace {

// SecureWebSocketClient keeps bounded RX/event buffers in the owning task's
// frame. Match the already hardware-validated Stage 0 transport probe stack so
// the TLS handshake cannot consume the canary margin.
constexpr uint32_t ProbeTaskStackBytes = 48U * 1024U;
constexpr uint32_t WiFiTimeoutMs = 30000;
constexpr uint32_t TimeSyncTimeoutMs = 30000;
constexpr uint32_t HelloTimeoutMs = 10000;
constexpr uint32_t DefaultActivationTimeoutMs = 10U * 60U * 1000U;
constexpr uint32_t MinimumActivationTimeoutMs = 30000;
constexpr uint32_t MaximumActivationTimeoutMs = 10U * 60U * 1000U;
constexpr uint8_t MaximumBootstrapAttempts = 8;
constexpr uint32_t BootstrapRetryDelayMs = 3000;
constexpr time_t MinimumValidEpoch = 1609459200;
constexpr char PreferencesNamespace[] = "assistant";
constexpr char PhaseKey[] = "s1_phase";
constexpr char McpModeKey[] = "s1_mcp";

enum class McpMode : uint8_t { Disabled = 0, Omitted = 1 };
enum class HelloAttempt : uint8_t { Passed, ConnectFailed, HelloFailed };

struct MemorySample {
  size_t freeInternal;
  size_t largestInternal;
  size_t freePsram;
};

struct ParsedEndpoint {
  char host[256];
  char path[XiaozhiWebSocketCredentials::MaxUrlBytes + 1];
  uint16_t port;
};

MemorySample memorySample() {
  return {
    heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
    heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
    ESP.getFreePsram()
  };
}

void printMemory(const char *label) {
  const MemorySample sample = memorySample();
  Serial.print(label);
  Serial.print(F(" free_internal="));
  Serial.print(sample.freeInternal);
  Serial.print(F(" largest_internal="));
  Serial.print(sample.largestInternal);
  Serial.print(F(" free_psram="));
  Serial.println(sample.freePsram);
}

bool connectSavedWiFi() {
  WiFi.useStaticBuffers(true);
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  const uint32_t started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < WiFiTimeoutMs) {
    delay(50);
  }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("FAIL wifi: saved network did not connect"));
    return false;
  }
  Serial.println(F("PASS wifi connected"));
  printMemory("WIFI_READY");
  return true;
}

bool synchronizeTime() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  const uint32_t started = millis();
  while (millis() - started < TimeSyncTimeoutMs) {
    if (time(nullptr) >= MinimumValidEpoch) {
      Serial.println(F("PASS time synchronized for TLS"));
      return true;
    }
    delay(100);
  }
  Serial.println(F("FAIL time: SNTP unavailable; secure bootstrap cannot run"));
  return false;
}

void printSanitized(const char *value) {
  if (!value) return;
  for (const char *cursor = value; *cursor; ++cursor) {
    const uint8_t byte = static_cast<uint8_t>(*cursor);
    if (byte == '\n' || (byte >= 0x20 && byte <= 0x7e)) Serial.write(byte);
    else Serial.write('?');
  }
}

bool loadProbeState(uint8_t &phase, McpMode &mode) {
  Preferences preferences;
  if (!preferences.begin(PreferencesNamespace, false)) return false;
  phase = preferences.getUChar(PhaseKey, 0);
  const uint8_t storedMode = preferences.getUChar(McpModeKey, 0xff);
  preferences.end();
  if (phase == 0) {
    mode = McpMode::Disabled;
    return true;
  }
  if (phase != 1 || storedMode > static_cast<uint8_t>(McpMode::Omitted)) return false;
  mode = static_cast<McpMode>(storedMode);
  return true;
}

bool saveProbeState(McpMode mode) {
  Preferences preferences;
  if (!preferences.begin(PreferencesNamespace, false)) return false;
  const bool phaseSaved = preferences.putUChar(PhaseKey, 1) == sizeof(uint8_t);
  const bool modeSaved = preferences.putUChar(McpModeKey, static_cast<uint8_t>(mode)) == sizeof(uint8_t);
  preferences.end();
  return phaseSaved && modeSaved;
}

bool clearProbeState() {
  Preferences preferences;
  if (!preferences.begin(PreferencesNamespace, false)) return false;
  const bool phaseCleared = !preferences.isKey(PhaseKey) || preferences.remove(PhaseKey);
  const bool modeCleared = !preferences.isKey(McpModeKey) || preferences.remove(McpModeKey);
  preferences.end();
  return phaseCleared && modeCleared;
}

bool parsePort(const char *value, size_t length, uint16_t &port) {
  if (!value || length == 0 || length > 5) return false;
  uint32_t parsed = 0;
  for (size_t i = 0; i < length; ++i) {
    if (value[i] < '0' || value[i] > '9') return false;
    parsed = parsed * 10U + static_cast<uint32_t>(value[i] - '0');
  }
  if (parsed == 0 || parsed > UINT16_MAX) return false;
  port = static_cast<uint16_t>(parsed);
  return true;
}

bool parseWssUrl(const char *url, ParsedEndpoint &endpoint) {
  memset(&endpoint, 0, sizeof(endpoint));
  endpoint.port = 443;
  if (!url || strncmp(url, "wss://", 6) != 0 || strlen(url) > XiaozhiWebSocketCredentials::MaxUrlBytes ||
      strchr(url, '#') || strchr(url + 6, '@')) {
    return false;
  }
  const char *authority = url + 6;
  const char *path = strchr(authority, '/');
  const char *query = strchr(authority, '?');
  if (!path || (query && query < path)) path = query;
  const char *authorityEnd = path ? path : url + strlen(url);
  const char *colon = nullptr;
  for (const char *cursor = authority; cursor < authorityEnd; ++cursor) {
    if (*cursor == ':') colon = cursor;
  }
  const char *hostEnd = colon ? colon : authorityEnd;
  const size_t hostLength = static_cast<size_t>(hostEnd - authority);
  if (hostLength == 0 || hostLength >= sizeof(endpoint.host)) return false;
  memcpy(endpoint.host, authority, hostLength);
  endpoint.host[hostLength] = 0;
  if (colon && !parsePort(colon + 1, static_cast<size_t>(authorityEnd - colon - 1), endpoint.port)) {
    return false;
  }
  if (path) {
    if (*path == '?') {
      endpoint.path[0] = '/';
      const size_t pathLength = strlen(path);
      if (pathLength + 1 >= sizeof(endpoint.path)) return false;
      memcpy(endpoint.path + 1, path, pathLength + 1);
    } else {
      if (strlen(path) >= sizeof(endpoint.path)) return false;
      strcpy(endpoint.path, path);
    }
  } else {
    strcpy(endpoint.path, "/");
  }
  return true;
}

String helloMessage(uint8_t version, McpMode mode) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return "";
  cJSON_AddStringToObject(root, "type", "hello");
  cJSON_AddNumberToObject(root, "version", version);
  if (mode == McpMode::Disabled) {
    cJSON *features = cJSON_AddObjectToObject(root, "features");
    cJSON_AddBoolToObject(features, "mcp", false);
  }
  cJSON_AddStringToObject(root, "transport", "websocket");
  cJSON *audio = cJSON_AddObjectToObject(root, "audio_params");
  cJSON_AddStringToObject(audio, "format", "opus");
  cJSON_AddNumberToObject(audio, "sample_rate", 16000);
  cJSON_AddNumberToObject(audio, "channels", 1);
  cJSON_AddNumberToObject(audio, "frame_duration", 60);
  char *serialized = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!serialized) return "";
  String output(serialized);
  cJSON_free(serialized);
  return output;
}

bool validServerHello(const uint8_t *payload, size_t length) {
  cJSON *root = cJSON_ParseWithLength(reinterpret_cast<const char *>(payload), length);
  if (!root) return false;
  cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  cJSON *transport = cJSON_GetObjectItemCaseSensitive(root, "transport");
  bool valid = cJSON_IsString(type) && strcmp(type->valuestring, "hello") == 0 &&
               cJSON_IsString(transport) && strcmp(transport->valuestring, "websocket") == 0;
  cJSON *audio = cJSON_GetObjectItemCaseSensitive(root, "audio_params");
  if (valid && audio && !cJSON_IsObject(audio)) valid = false;
  if (valid && cJSON_IsObject(audio)) {
    cJSON *format = cJSON_GetObjectItemCaseSensitive(audio, "format");
    cJSON *sampleRate = cJSON_GetObjectItemCaseSensitive(audio, "sample_rate");
    cJSON *channels = cJSON_GetObjectItemCaseSensitive(audio, "channels");
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(audio, "frame_duration");
    if (format && (!cJSON_IsString(format) || strcmp(format->valuestring, "opus") != 0)) valid = false;
    if (sampleRate && (!cJSON_IsNumber(sampleRate) ||
        (sampleRate->valueint != 16000 && sampleRate->valueint != 24000))) valid = false;
    if (channels && (!cJSON_IsNumber(channels) || channels->valueint != 1)) valid = false;
    if (duration && (!cJSON_IsNumber(duration) || duration->valueint != 60)) valid = false;
  }
  cJSON_Delete(root);
  return valid;
}

HelloAttempt attemptHello(const XiaozhiIdentity &identity,
                          const XiaozhiWebSocketCredentials &credentials,
                          McpMode mode) {
  ParsedEndpoint parsed = {};
  if (!parseWssUrl(credentials.url, parsed)) {
    Serial.println(F("FAIL wss: bootstrap returned an invalid or non-WSS endpoint"));
    return HelloAttempt::ConnectFailed;
  }
  char authorization[XiaozhiWebSocketCredentials::MaxTokenBytes + 8] = {};
  if (strchr(credentials.token, ' ')) snprintf(authorization, sizeof(authorization), "%s", credentials.token);
  else snprintf(authorization, sizeof(authorization), "Bearer %s", credentials.token);
  char version[4] = {};
  snprintf(version, sizeof(version), "%u", credentials.version);
  const SecureWebSocketClient::Header headers[] = {
    {"Authorization", authorization},
    {"Protocol-Version", version},
    {"Device-Id", identity.deviceId()},
    {"Client-Id", identity.clientId()}
  };
  const SecureWebSocketClient::Endpoint endpoint = {parsed.host, parsed.port, parsed.path, ""};
  const SecureWebSocketClient::TrustStore trust = {
    SecureWebSocketClient::TrustKind::PemCertificate,
    reinterpret_cast<const uint8_t *>(XiaozhiProbeConfig::CaCertificate),
    strlen(XiaozhiProbeConfig::CaCertificate) + 1U
  };

  SecureWebSocketClient websocket;
  if (!websocket.connect(endpoint, headers, sizeof(headers) / sizeof(headers[0]), trust)) {
    memset(authorization, 0, sizeof(authorization));
    Serial.print(F("FAIL wss connect: "));
    Serial.println(websocket.lastError());
    return HelloAttempt::ConnectFailed;
  }
  memset(authorization, 0, sizeof(authorization));
  String hello = helloMessage(credentials.version, mode);
  if (hello.length() == 0 || !websocket.sendText(hello.c_str(), hello.length())) {
    hello = "";
    websocket.close();
    Serial.println(F("FAIL wss: client hello could not be sent"));
    return HelloAttempt::HelloFailed;
  }
  hello = "";

  SecureWebSocketClient::Event event = {};
  const uint32_t started = millis();
  while (millis() - started < HelloTimeoutMs) {
    if (!websocket.poll(event)) {
      delay(1);
      continue;
    }
    if (event.type == SecureWebSocketClient::EventType::Text &&
        validServerHello(event.payload, event.length)) {
      websocket.close();
      memset(&event, 0, sizeof(event));
      Serial.println(F("PASS wss server hello accepted"));
      return HelloAttempt::Passed;
    }
    if (event.type == SecureWebSocketClient::EventType::Error ||
        event.type == SecureWebSocketClient::EventType::Disconnected ||
        event.type == SecureWebSocketClient::EventType::Overflow) {
      websocket.close();
      memset(&event, 0, sizeof(event));
      Serial.println(F("FAIL wss: connection ended before server hello"));
      return HelloAttempt::HelloFailed;
    }
  }
  websocket.close();
  memset(&event, 0, sizeof(event));
  Serial.println(F("FAIL wss: server hello timed out"));
  return HelloAttempt::HelloFailed;
}

bool waitForActivation(const XiaozhiIdentity &identity,
                       XiaozhiBootstrapClient &bootstrap,
                       const XiaozhiActivationInstructions &instructions) {
  Serial.println(F("ACTION REQUIRED: bind this device in the Xiaozhi console"));
  if (instructions.message[0]) {
    Serial.print(F("Activation instructions: "));
    printSanitized(instructions.message);
    Serial.println();
  }
  Serial.print(F("Activation code: "));
  printSanitized(instructions.code);
  Serial.println();

  uint32_t timeoutMs = instructions.timeoutMs;
  if (timeoutMs < MinimumActivationTimeoutMs || timeoutMs > MaximumActivationTimeoutMs) {
    timeoutMs = DefaultActivationTimeoutMs;
  }
  const uint32_t started = millis();
  uint32_t retryDelayMs = 3000;
  while (millis() - started < timeoutMs) {
    const XiaozhiActivationPoll result = bootstrap.activate(identity);
    Serial.print(F("Activation poll HTTP status="));
    Serial.println(bootstrap.lastHttpStatus());
    if (result == XiaozhiActivationPoll::Accepted) {
      Serial.println(F("PASS account binding accepted"));
      return true;
    }
    if (result == XiaozhiActivationPoll::FatalError) {
      Serial.print(F("FAIL activation: "));
      Serial.println(bootstrap.lastError());
      return false;
    }
    if (result == XiaozhiActivationPoll::RetryableError) {
      retryDelayMs = min<uint32_t>(retryDelayMs * 2U, 10000U);
    } else {
      retryDelayMs = 3000;
    }
    delay(retryDelayMs);
  }
  Serial.println(F("FAIL activation: binding timed out"));
  return false;
}

bool obtainCredentials(const XiaozhiIdentity &identity,
                       XiaozhiBootstrapClient &bootstrap,
                       XiaozhiBootstrapResponse &response) {
  for (uint8_t check = 0; check < MaximumBootstrapAttempts; ++check) {
    response.clear();
    if (!bootstrap.check(identity, response)) {
      Serial.print(F("FAIL bootstrap status="));
      Serial.print(bootstrap.lastHttpStatus());
      Serial.print(F(" error="));
      Serial.println(bootstrap.lastError());
      return false;
    }
    Serial.print(F("PASS bootstrap HTTP 200 protocol_version="));
    Serial.println(response.websocket.version);
    if (response.activation.present) {
      if (!waitForActivation(identity, bootstrap, response.activation)) return false;
      response.clear();
      delay(1000);
      continue;
    }
    const XiaozhiWebSocketCredentialStatus credentialStatus = response.websocket.status();
    if (credentialStatus == XiaozhiWebSocketCredentialStatus::PlaceholderToken) {
      Serial.println(F("FAIL bootstrap: hosted service returned a placeholder/test WebSocket token"));
      return false;
    }
    if (credentialStatus == XiaozhiWebSocketCredentialStatus::UnsupportedProtocol) {
      Serial.println(F("FAIL bootstrap: hosted service returned an unsupported protocol version"));
      return false;
    }
    if (credentialStatus == XiaozhiWebSocketCredentialStatus::Missing) {
      Serial.println(F("WAIT bootstrap: WebSocket URL or token is absent; retrying"));
      if (check + 1 < MaximumBootstrapAttempts) delay(BootstrapRetryDelayMs);
      continue;
    }
    Serial.println(F("PASS bootstrap returned usable non-placeholder credentials"));
    return true;
  }
  Serial.println(F("FAIL bootstrap: usable WebSocket credentials did not become available"));
  return false;
}

void runXiaozhiCompatibilityProbe() {
  Serial.println(F("ESP Media Stage 1 Xiaozhi compatibility probe"));
  Serial.print(F("Target="));
  Serial.println(XiaozhiProbeConfig::TargetName);
  Serial.println(F("Upstream snapshot: 78/xiaozhi-esp32 ac6deed3"));
  printMemory("START");

  if (!connectSavedWiFi() || !synchronizeTime()) {
    Serial.println(F("STAGE1_PROBE FAIL"));
    return;
  }

  XiaozhiIdentity identity;
  if (!identity.begin()) {
    Serial.print(F("FAIL identity: "));
    Serial.println(identity.lastError());
    Serial.println(F("STAGE1_PROBE FAIL"));
    return;
  }
  Serial.print(F("Device-Id="));
  Serial.println(identity.deviceId());
  Serial.print(F("Client-Id="));
  Serial.println(identity.clientId());
  Serial.println(F("PASS stable device identity loaded"));

  uint8_t phase = 0;
  McpMode selectedMode = McpMode::Disabled;
  if (!loadProbeState(phase, selectedMode)) {
    Serial.println(F("FAIL probe: malformed automatic-reboot state"));
    Serial.println(F("STAGE1_PROBE FAIL"));
    return;
  }
  Serial.println(phase == 1 ? F("Second-boot credential refresh gate")
                            : F("First-boot binding and handshake gate"));

  const XiaozhiBootstrapEndpoint endpoint = {
    XiaozhiProbeConfig::BootstrapUrl,
    XiaozhiProbeConfig::ActivationUrl,
    XiaozhiProbeConfig::CaCertificate
  };
  XiaozhiBootstrapClient bootstrap(endpoint);
  XiaozhiBootstrapResponse response;
  if (!obtainCredentials(identity, bootstrap, response)) {
    response.clear();
    Serial.println(F("STAGE1_PROBE FAIL"));
    return;
  }

  HelloAttempt helloResult;
  if (phase == 1) {
    helloResult = attemptHello(identity, response.websocket, selectedMode);
  } else {
    selectedMode = McpMode::Disabled;
    helloResult = attemptHello(identity, response.websocket, selectedMode);
    if (helloResult == HelloAttempt::HelloFailed) {
      Serial.println(F("Retrying client hello with MCP capability omitted"));
      selectedMode = McpMode::Omitted;
      helloResult = attemptHello(identity, response.websocket, selectedMode);
    }
  }
  response.clear();
  if (helloResult != HelloAttempt::Passed) {
    Serial.println(F("STAGE1_PROBE FAIL"));
    return;
  }

  Serial.print(F("PASS MCP mode="));
  Serial.println(selectedMode == McpMode::Disabled ? F("disabled") : F("omitted"));
  printMemory("WSS_CLOSED");
  Serial.print(F("probe_task_stack_high_water_bytes="));
  Serial.println(uxTaskGetStackHighWaterMark(nullptr));

  if (phase == 0) {
    if (!saveProbeState(selectedMode)) {
      Serial.println(F("FAIL probe: cannot persist automatic-reboot marker"));
      Serial.println(F("STAGE1_PROBE FAIL"));
      return;
    }
    Serial.println(F("PASS first boot; restarting for credential refresh gate"));
    Serial.flush();
    delay(2000);
    ESP.restart();
    return;
  }

  if (!clearProbeState()) {
    Serial.println(F("FAIL probe: cannot clear automatic-reboot marker"));
    Serial.println(F("STAGE1_PROBE FAIL"));
    return;
  }
  Serial.println(F("PASS fresh bootstrap credentials worked after reboot"));
  Serial.println(F("STAGE1_PROBE PASS"));
}

void xiaozhiCompatibilityProbeTask(void *) {
  runXiaozhiCompatibilityProbe();
  vTaskDelete(nullptr);
}

} // namespace

void xiaozhiCompatibilityProbeSetup() {
  Serial.begin(115200);
  const uint32_t started = millis();
  while (!Serial && millis() - started < 2000U) delay(10);
  const BaseType_t created = xTaskCreatePinnedToCore(xiaozhiCompatibilityProbeTask,
                                                     "xiaozhiProbe",
                                                     ProbeTaskStackBytes,
                                                     nullptr,
                                                     tskIDLE_PRIORITY + 1,
                                                     nullptr,
                                                     xPortGetCoreID());
  if (created != pdPASS) Serial.println(F("FAIL probe task creation"));
}

void xiaozhiCompatibilityProbeLoop() {
  delay(1000);
}
