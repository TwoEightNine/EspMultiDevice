#include "XiaozhiBootstrapClient.h"

#include "XiaozhiIdentity.h"
#include "XiaozhiTrust.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <cJSON.h>
#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <string.h>

namespace {
constexpr uint32_t HttpTimeoutMs = 15000;
constexpr char UserAgentPrefix[] = "esp-media/";

bool equalsIgnoreCase(const char *left, const char *right) {
  if (!left || !right) return false;
  while (*left && *right) {
    char a = *left++;
    char b = *right++;
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return *left == 0 && *right == 0;
}

bool startsWithIgnoreCase(const char *value, const char *prefix) {
  if (!value || !prefix) return false;
  if (strlen(value) < strlen(prefix)) return false;
  while (*prefix) {
    char a = *value++;
    char b = *prefix++;
    if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
    if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
    if (a != b) return false;
  }
  return true;
}

void addString(cJSON *object, const char *name, const String &value) {
  cJSON_AddStringToObject(object, name, value.c_str());
}

void wipeJsonStrings(cJSON *node) {
  if (!node) return;
  for (cJSON *child = node->child; child; child = child->next) wipeJsonStrings(child);
  if (cJSON_IsString(node) && node->valuestring) {
    memset(node->valuestring, 0, strlen(node->valuestring));
  }
}

void secureDelete(cJSON *node) {
  wipeJsonStrings(node);
  cJSON_Delete(node);
}
} // namespace

void XiaozhiActivationInstructions::clear() {
  present = false;
  memset(code, 0, sizeof(code));
  memset(message, 0, sizeof(message));
  timeoutMs = 0;
}

void XiaozhiWebSocketCredentials::clear() {
  present = false;
  memset(url, 0, sizeof(url));
  memset(token, 0, sizeof(token));
  version = 1;
}

XiaozhiWebSocketCredentialStatus XiaozhiWebSocketCredentials::status() const {
  if (!present || url[0] == 0 || token[0] == 0) {
    return XiaozhiWebSocketCredentialStatus::Missing;
  }
  if (version < 1 || version > 3) {
    return XiaozhiWebSocketCredentialStatus::UnsupportedProtocol;
  }
  const char *tokenValue = startsWithIgnoreCase(token, "bearer ") ? token + 7 : token;
  static constexpr const char *Placeholders[] = {
    "test-token", "test_token", "placeholder", "your-token", "token"
  };
  for (const char *placeholder : Placeholders) {
    if (equalsIgnoreCase(tokenValue, placeholder)) {
      return XiaozhiWebSocketCredentialStatus::PlaceholderToken;
    }
  }
  return XiaozhiWebSocketCredentialStatus::Usable;
}

bool XiaozhiWebSocketCredentials::usable() const {
  return status() == XiaozhiWebSocketCredentialStatus::Usable;
}

void XiaozhiBootstrapResponse::clear() {
  activation.clear();
  websocket.clear();
}

XiaozhiBootstrapClient::XiaozhiBootstrapClient()
    : _endpoint{OfficialBootstrapUrl, OfficialActivationUrl,
                XiaozhiTrust::DigiCertGlobalRootG2} {}

XiaozhiBootstrapClient::XiaozhiBootstrapClient(const XiaozhiBootstrapEndpoint &endpoint)
    : _endpoint(endpoint) {}

bool XiaozhiBootstrapClient::check(const XiaozhiIdentity &identity,
                                   XiaozhiBootstrapResponse &response) {
  response.clear();
  String request;
  if (!buildSystemInfoJson(identity, request)) return false;

  char *body = static_cast<char *>(heap_caps_malloc(MaxResponseBytes + 1,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!body) body = static_cast<char *>(malloc(MaxResponseBytes + 1));
  if (!body) {
    _lastError = F("Cannot allocate bounded bootstrap response");
    return false;
  }
  size_t length = 0;
  const bool posted = post(_endpoint.bootstrapUrl, identity, request.c_str(), body,
                           MaxResponseBytes + 1, length);
  request = "";
  bool parsed = false;
  if (posted && _lastHttpStatus == HTTP_CODE_OK) {
    parsed = parseResponse(body, length, response);
  } else if (posted) {
    _lastError = F("Bootstrap HTTP status was not 200");
  }
  memset(body, 0, MaxResponseBytes + 1);
  free(body);
  return posted && _lastHttpStatus == HTTP_CODE_OK && parsed;
}

XiaozhiActivationPoll XiaozhiBootstrapClient::activate(const XiaozhiIdentity &identity) {
  char response[513] = {};
  size_t responseLength = 0;
  if (!post(_endpoint.activationUrl, identity, "{}", response, sizeof(response), responseLength)) {
    memset(response, 0, sizeof(response));
    return XiaozhiActivationPoll::RetryableError;
  }
  memset(response, 0, sizeof(response));
  if (_lastHttpStatus == HTTP_CODE_OK) return XiaozhiActivationPoll::Accepted;
  if (_lastHttpStatus == HTTP_CODE_ACCEPTED) return XiaozhiActivationPoll::Pending;
  if (_lastHttpStatus >= 500) return XiaozhiActivationPoll::RetryableError;
  _lastError = F("Activation endpoint rejected the request");
  return XiaozhiActivationPoll::FatalError;
}

bool XiaozhiBootstrapClient::post(const char *url, const XiaozhiIdentity &identity,
                                  const char *body, char *response,
                                  size_t responseCapacity, size_t &responseLength) {
  responseLength = 0;
  _lastHttpStatus = 0;
  if (!url || !startsWithIgnoreCase(url, "https://") || !_endpoint.caCertificate ||
      _endpoint.caCertificate[0] == 0 || !body || !response || responseCapacity < 2) {
    _lastError = F("Invalid bootstrap request");
    return false;
  }

  NetworkClientSecure tls;
  tls.setCACert(_endpoint.caCertificate);
  tls.setHandshakeTimeout(HttpTimeoutMs / 1000U);
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(HttpTimeoutMs);
  http.setTimeout(HttpTimeoutMs);
  if (!http.begin(tls, url)) {
    _lastError = F("Cannot start secure bootstrap HTTP client");
    return false;
  }

  const esp_app_desc_t *app = esp_app_get_description();
  String userAgent = UserAgentPrefix;
  userAgent += app && app->version[0] ? app->version : "unknown";
  http.setUserAgent(userAgent);
  http.addHeader(F("Activation-Version"), F("1"));
  http.addHeader(F("Device-Id"), identity.deviceId());
  http.addHeader(F("Client-Id"), identity.clientId());
  http.addHeader(F("Accept-Language"), F("en-US"));
  http.addHeader(F("Content-Type"), F("application/json"));

  _lastHttpStatus = http.POST(reinterpret_cast<uint8_t *>(const_cast<char *>(body)), strlen(body));
  if (_lastHttpStatus <= 0) {
    _lastError = F("Secure bootstrap POST failed");
    http.end();
    return false;
  }
  const bool read = readBoundedBody(http, response, responseCapacity, responseLength);
  http.end();
  if (!read) return false;
  _lastError = F("OK");
  return true;
}

bool XiaozhiBootstrapClient::readBoundedBody(HTTPClient &http, char *response,
                                             size_t responseCapacity,
                                             size_t &responseLength) {
  const int declared = http.getSize();
  if (declared >= 0 && static_cast<size_t>(declared) >= responseCapacity) {
    _lastError = F("Bootstrap response exceeds limit");
    return false;
  }
  NetworkClient *stream = http.getStreamPtr();
  if (!stream) {
    _lastError = F("Bootstrap response stream unavailable");
    return false;
  }

  responseLength = 0;
  uint32_t lastProgress = millis();
  int remaining = declared;
  while ((remaining != 0) && (http.connected() || stream->available())) {
    const int available = stream->available();
    if (available <= 0) {
      if (millis() - lastProgress >= HttpTimeoutMs) {
        _lastError = F("Bootstrap response timed out");
        return false;
      }
      delay(1);
      continue;
    }
    size_t amount = static_cast<size_t>(available);
    if (remaining > 0 && amount > static_cast<size_t>(remaining)) amount = remaining;
    if (amount >= responseCapacity - responseLength) {
      _lastError = F("Bootstrap response exceeds limit");
      return false;
    }
    const int received = stream->read(reinterpret_cast<uint8_t *>(response + responseLength), amount);
    if (received <= 0) continue;
    responseLength += static_cast<size_t>(received);
    if (remaining > 0) remaining -= received;
    lastProgress = millis();
  }
  if (remaining > 0) {
    _lastError = F("Bootstrap response ended early");
    return false;
  }
  response[responseLength] = 0;
  return true;
}

bool XiaozhiBootstrapClient::buildSystemInfoJson(const XiaozhiIdentity &identity,
                                                 String &output) {
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    _lastError = F("Cannot allocate bootstrap JSON");
    return false;
  }
  cJSON_AddNumberToObject(root, "version", 2);
  cJSON_AddStringToObject(root, "language", "en-US");
  cJSON_AddNumberToObject(root, "flash_size", ESP.getFlashChipSize());
  cJSON_AddNumberToObject(root, "psram_size", ESP.getPsramSize());
  char minimumHeap[24];
  snprintf(minimumHeap, sizeof(minimumHeap), "%u",
           static_cast<unsigned>(esp_get_minimum_free_heap_size()));
  cJSON_AddStringToObject(root, "minimum_free_heap_size", minimumHeap);
  cJSON_AddStringToObject(root, "mac_address", identity.deviceId());
  cJSON_AddStringToObject(root, "uuid", identity.clientId());
  cJSON_AddStringToObject(root, "chip_model_name", "esp32s3");

  esp_chip_info_t chip = {};
  esp_chip_info(&chip);
  cJSON *chipJson = cJSON_AddObjectToObject(root, "chip_info");
  cJSON_AddNumberToObject(chipJson, "model", chip.model);
  cJSON_AddNumberToObject(chipJson, "cores", chip.cores);
  cJSON_AddNumberToObject(chipJson, "revision", chip.revision);
  cJSON_AddNumberToObject(chipJson, "features", chip.features);

  const esp_app_desc_t *app = esp_app_get_description();
  cJSON *application = cJSON_AddObjectToObject(root, "application");
  cJSON_AddStringToObject(application, "name", "esp-media");
  cJSON_AddStringToObject(application, "version", app && app->version[0] ? app->version : "unknown");
  String compileTime = app ? String(app->date) + 'T' + app->time + 'Z' : String("unknown");
  addString(application, "compile_time", compileTime);
  cJSON_AddStringToObject(application, "idf_version", app ? app->idf_ver : ESP.getSdkVersion());
  char sha256[65] = {};
  if (app) {
    for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i) {
      snprintf(sha256 + i * 2, sizeof(sha256) - i * 2, "%02x", app->app_elf_sha256[i]);
    }
  }
  cJSON_AddStringToObject(application, "elf_sha256", sha256);

  cJSON *partitions = cJSON_AddArrayToObject(root, "partition_table");
  esp_partition_iterator_t iterator = esp_partition_find(ESP_PARTITION_TYPE_ANY,
                                                          ESP_PARTITION_SUBTYPE_ANY, nullptr);
  while (iterator) {
    const esp_partition_t *partition = esp_partition_get(iterator);
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "label", partition->label);
    cJSON_AddNumberToObject(item, "type", partition->type);
    cJSON_AddNumberToObject(item, "subtype", partition->subtype);
    cJSON_AddNumberToObject(item, "address", partition->address);
    cJSON_AddNumberToObject(item, "size", partition->size);
    cJSON_AddItemToArray(partitions, item);
    iterator = esp_partition_next(iterator);
  }

  cJSON *ota = cJSON_AddObjectToObject(root, "ota");
  const esp_partition_t *running = esp_ota_get_running_partition();
  cJSON_AddStringToObject(ota, "label", running ? running->label : "unknown");

  cJSON *display = cJSON_AddObjectToObject(root, "display");
  cJSON_AddBoolToObject(display, "monochrome", false);
  cJSON_AddNumberToObject(display, "width", 480);
  cJSON_AddNumberToObject(display, "height", 320);

  cJSON *board = cJSON_AddObjectToObject(root, "board");
  cJSON_AddStringToObject(board, "type", "wifi");
  cJSON_AddStringToObject(board, "name", "esp-media");
  cJSON_AddStringToObject(board, "manufacturer", "custom");
  addString(board, "ssid", WiFi.SSID());
  cJSON_AddNumberToObject(board, "rssi", WiFi.RSSI());
  cJSON_AddNumberToObject(board, "channel", WiFi.channel());
  addString(board, "ip", WiFi.localIP().toString());
  cJSON_AddStringToObject(board, "mac", identity.deviceId());

  char *serialized = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!serialized) {
    _lastError = F("Cannot serialize bootstrap JSON");
    return false;
  }
  output = serialized;
  cJSON_free(serialized);
  if (output.length() == 0 || output.length() > MaxResponseBytes) {
    output = "";
    _lastError = F("Bootstrap request exceeds limit");
    return false;
  }
  return true;
}

bool XiaozhiBootstrapClient::parseResponse(char *body, size_t length,
                                           XiaozhiBootstrapResponse &response) {
  cJSON *root = cJSON_ParseWithLength(body, length);
  if (!root || !cJSON_IsObject(root)) {
    secureDelete(root);
    _lastError = F("Bootstrap response is not valid JSON");
    return false;
  }

  cJSON *activation = cJSON_GetObjectItemCaseSensitive(root, "activation");
  if (cJSON_IsObject(activation)) {
    response.activation.present = true;
    if (!copyJsonString(activation, "code", response.activation.code,
                        sizeof(response.activation.code)) ||
        !copyJsonString(activation, "message", response.activation.message,
                        sizeof(response.activation.message))) {
      secureDelete(root);
      response.clear();
      _lastError = F("Activation instructions exceed limits");
      return false;
    }
    if (response.activation.code[0] == 0) {
      secureDelete(root);
      response.clear();
      _lastError = F("Activation instructions are missing a code");
      return false;
    }
    cJSON *timeout = cJSON_GetObjectItemCaseSensitive(activation, "timeout_ms");
    if (cJSON_IsNumber(timeout) && timeout->valuedouble >= 0 &&
        timeout->valuedouble <= UINT32_MAX) {
      response.activation.timeoutMs = static_cast<uint32_t>(timeout->valuedouble);
    }
  }

  cJSON *websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
  if (cJSON_IsObject(websocket)) {
    response.websocket.present = true;
    if (!copyJsonString(websocket, "url", response.websocket.url,
                        sizeof(response.websocket.url)) ||
        !copyJsonString(websocket, "token", response.websocket.token,
                        sizeof(response.websocket.token))) {
      secureDelete(root);
      response.clear();
      _lastError = F("WebSocket credentials exceed limits");
      return false;
    }
    cJSON *version = cJSON_GetObjectItemCaseSensitive(websocket, "version");
    if (cJSON_IsNumber(version) && version->valueint != 0) {
      if (version->valueint < 1 || version->valueint > 3) {
        secureDelete(root);
        response.clear();
        _lastError = F("Unsupported Xiaozhi protocol version");
        return false;
      }
      response.websocket.version = static_cast<uint8_t>(version->valueint);
    }
  }

  secureDelete(root);
  _lastError = F("OK");
  return true;
}

bool XiaozhiBootstrapClient::copyJsonString(void *objectValue, const char *name,
                                            char *output, size_t capacity) {
  cJSON *object = static_cast<cJSON *>(objectValue);
  cJSON *value = cJSON_GetObjectItemCaseSensitive(object, name);
  if (!value) {
    output[0] = 0;
    return true;
  }
  if (!cJSON_IsString(value) || !value->valuestring) return false;
  const size_t length = strlen(value->valuestring);
  if (length >= capacity) return false;
  memcpy(output, value->valuestring, length + 1);
  return true;
}
