#pragma once

#include <Arduino.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

// A bounded HTTP/1.1 client for one request at a time. HTTPS validates the
// server against the certificate bundle linked into the firmware, so the
// clock must be set. The caller supplies the request, a host policy that also
// observes cancellation, and a sink that receives the decoded body. The
// client never draws, navigates, touches SD, or logs paths, queries, headers,
// or bodies.
class HttpClient {
public:
  static constexpr uint8_t MaxRequestHeaders = 16;
  static constexpr uint8_t MaxResponseHeaders = 32;
  static constexpr size_t MaxResponseHeaderBytes = 4096;
  static constexpr size_t MaxUrlBytes = 2048;
  static constexpr uint8_t MaxRedirects = 5;

  enum class Method : uint8_t { Get, Post, Put, Patch, Delete };

  struct Url {
    String scheme;  // "http" or "https"
    String host;    // lowercase
    uint16_t port = 0;
    String target;  // path and query, never empty
  };

  struct Header {
    String name;   // lowercase for responses
    String value;
  };

  struct Request {
    Method method = Method::Get;
    String url;
    const Header *headers = nullptr;
    uint8_t headerCount = 0;
    const uint8_t *body = nullptr;
    size_t bodyLength = 0;
    bool followRedirects = false;
    uint32_t timeoutMs = 20000;
    // When set, the body may outlast `timeoutMs` as long as data keeps
    // arriving at least this often (long downloads).
    uint32_t idleTimeoutMs = 0;
    String userAgent;
  };

  struct Response {
    int status = 0;
    String finalUrl;
    String contentType;  // lowercase
    Header headers[MaxResponseHeaders];
    uint8_t headerCount = 0;
    bool headersTruncated = false;
    String error;  // empty on transport success, whatever the status
  };

  class Policy {
  public:
    virtual ~Policy() = default;
    virtual bool cancelled() const = 0;
    virtual bool hostAllowed(const Url &url) const = 0;
    // `total` is -1 while unknown; `received` counts body bytes.
    virtual void progress(uint8_t phase, uint32_t received, int32_t total) = 0;
  };

  // Receives the body of the final response. Returning false from `begin` or
  // `write` stops the transfer; the sink keeps the reason.
  class Sink {
  public:
    virtual ~Sink() = default;
    virtual bool begin(int status, const String &contentType, int32_t contentLength) = 0;
    virtual bool write(const uint8_t *data, size_t length) = 0;
    virtual bool finish() = 0;
  };

  enum Phase : uint8_t { Connecting = 1, Sending = 2, Receiving = 3 };

  HttpClient();
  ~HttpClient();
  bool begin();
  void end();
  // Blocking; run it from a worker task. Returns false with response.error
  // set when no complete response arrived.
  bool perform(const Request &request, Policy &policy, Sink &sink, Response &response);
  // Closes an idle kept-alive connection.
  void closeIdle();

  static bool parseUrl(const String &value, Url &url);
  static bool resolve(const Url &base, const String &reference, Url &url);
  static String urlText(const Url &url);
  static const char *methodName(Method method);
  static bool validHeaderName(const char *name, size_t length);
  static bool validHeaderValue(const char *value, size_t length);
  static bool reservedHeader(const String &lowercaseName);

private:
  WiFiClient _plain;
  WiFiClientSecure _secure;
  WiFiClient *_open = nullptr;
  Url _openUrl;
  uint8_t *_buffer = nullptr;
  size_t _bufferBytes = 0;
  uint32_t _idleTimeoutMs = 0;  // of the request being performed

  struct Exchange {
    int status = 0;
    int32_t contentLength = -1;
    bool chunked = false;
    bool close = false;
    bool encoded = false;
    String location;
  };

  bool connect(const Url &url, Policy &policy, uint32_t deadline, String &error, bool &reused);
  bool sendRequest(WiFiClient &client, const Url &url, const Request &request,
                   Method method, const uint8_t *body, size_t bodyLength);
  bool readHead(WiFiClient &client, Policy &policy, uint32_t deadline,
                Exchange &exchange, Response &response, String &error);
  bool readBody(WiFiClient &client, Policy &policy, uint32_t deadline,
                const Exchange &exchange, Sink &sink, bool &consumed, String &error);
  bool readLine(WiFiClient &client, Policy &policy, uint32_t deadline, String &line,
                bool &tooLong);
  int readSome(WiFiClient &client, Policy &policy, uint32_t deadline,
               uint8_t *buffer, size_t capacity);
  void dropConnection();
  static bool sameOrigin(const Url &a, const Url &b);
};
