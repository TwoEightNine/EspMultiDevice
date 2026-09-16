#pragma once

#include <Arduino.h>

class HTTPClient;
class AssistantIdentity;

struct XiaozhiActivationInstructions {
  static constexpr size_t MaxCodeBytes = 32;
  static constexpr size_t MaxMessageBytes = 512;

  bool present;
  char code[MaxCodeBytes + 1];
  char message[MaxMessageBytes + 1];
  uint32_t timeoutMs;

  void clear();
};

enum class XiaozhiWebSocketCredentialStatus : uint8_t {
  Usable,
  Missing,
  PlaceholderToken,
  UnsupportedProtocol
};

struct XiaozhiWebSocketCredentials {
  static constexpr size_t MaxUrlBytes = 512;
  static constexpr size_t MaxTokenBytes = 768;

  bool present;
  char url[MaxUrlBytes + 1];
  char token[MaxTokenBytes + 1];
  uint8_t version;

  void clear();
  XiaozhiWebSocketCredentialStatus status() const;
  bool usable() const;
};

struct XiaozhiBootstrapResponse {
  XiaozhiActivationInstructions activation;
  XiaozhiWebSocketCredentials websocket;

  void clear();
};

enum class XiaozhiActivationPoll : uint8_t {
  Accepted,
  Pending,
  RetryableError,
  FatalError
};

struct AssistantBackendConfig {
  // These pointers are borrowed and must outlive the client.
  const char *bootstrapUrl;
  const char *activationUrl;
  const char *caCertificate;
  size_t caCertificateLength;
};

class XiaozhiBootstrap {
public:
  static constexpr size_t MaxResponseBytes = 16U * 1024U;

  explicit XiaozhiBootstrap(const AssistantBackendConfig &config);
  bool validConfiguration() const;
  bool check(const AssistantIdentity &identity, XiaozhiBootstrapResponse &response);
  XiaozhiActivationPoll activate(const AssistantIdentity &identity);
  const String &lastError() const { return _lastError; }
  int lastHttpStatus() const { return _lastHttpStatus; }
  // Numeric TLS transport failure only; never expose certificate or endpoint details.
  int lastTransportError() const { return _lastTransportError; }

private:
  AssistantBackendConfig _config;
  String _lastError;
  int _lastHttpStatus = 0;
  int _lastTransportError = 0;

  bool post(const char *url, const AssistantIdentity &identity, const char *body,
            char *response, size_t responseCapacity, size_t &responseLength);
  bool readBoundedBody(HTTPClient &http, char *response, size_t responseCapacity,
                       size_t &responseLength);
  bool buildSystemInfoJson(const AssistantIdentity &identity, String &output);
  bool parseResponse(char *body, size_t length, XiaozhiBootstrapResponse &response);
  static bool copyJsonString(void *object, const char *name, char *output, size_t capacity);
};
