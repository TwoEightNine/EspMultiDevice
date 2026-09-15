#pragma once

#include <Arduino.h>

class HTTPClient;
class XiaozhiIdentity;

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

struct XiaozhiBootstrapEndpoint {
  // These pointers are borrowed and must outlive the client.
  const char *bootstrapUrl;
  const char *activationUrl;
  const char *caCertificate;
};

class XiaozhiBootstrapClient {
public:
  static constexpr char OfficialBootstrapUrl[] = "https://api.tenclass.net/xiaozhi/ota/";
  static constexpr char OfficialActivationUrl[] = "https://api.tenclass.net/xiaozhi/ota/activate";
  static constexpr size_t MaxResponseBytes = 16U * 1024U;

  XiaozhiBootstrapClient();
  explicit XiaozhiBootstrapClient(const XiaozhiBootstrapEndpoint &endpoint);
  bool check(const XiaozhiIdentity &identity, XiaozhiBootstrapResponse &response);
  XiaozhiActivationPoll activate(const XiaozhiIdentity &identity);
  const String &lastError() const { return _lastError; }
  int lastHttpStatus() const { return _lastHttpStatus; }

private:
  XiaozhiBootstrapEndpoint _endpoint;
  String _lastError;
  int _lastHttpStatus = 0;

  bool post(const char *url, const XiaozhiIdentity &identity, const char *body,
            char *response, size_t responseCapacity, size_t &responseLength);
  bool readBoundedBody(HTTPClient &http, char *response, size_t responseCapacity,
                       size_t &responseLength);
  bool buildSystemInfoJson(const XiaozhiIdentity &identity, String &output);
  bool parseResponse(char *body, size_t length, XiaozhiBootstrapResponse &response);
  static bool copyJsonString(void *object, const char *name, char *output, size_t capacity);
};
