#include "HttpClient.h"

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "TlsMemoryScope.h"

// The full Mozilla root bundle that ESP-IDF's mbedTLS component links into
// every Arduino-ESP32 3.x image.
extern const uint8_t x509CertificateBundleStart[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509CertificateBundleEnd[] asm("_binary_x509_crt_bundle_end");

namespace {
constexpr uint32_t ConnectTimeoutMs = 10000;
constexpr uint32_t ValidClockEpoch = 1609459200;  // 2021-01-01
constexpr size_t MaxLineBytes = 2048;
constexpr uint8_t MaxHeaderLines = 64;

uint32_t remainingMs(uint32_t deadline) {
  const int32_t left = static_cast<int32_t>(deadline - millis());
  return left > 0 ? static_cast<uint32_t>(left) : 0;
}

String trimmed(const String &value) {
  String copy = value;
  copy.trim();
  return copy;
}

String lowercase(const String &value) {
  String copy = value;
  copy.toLowerCase();
  return copy;
}

void log(const String &line) { Serial.println(String("[App net] ") + line); }
}

HttpClient::HttpClient() {}

HttpClient::~HttpClient() { end(); }

bool HttpClient::begin() {
  if (!_buffer) {
    _bufferBytes = 8192;
    _buffer = static_cast<uint8_t *>(heap_caps_malloc(_bufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_buffer) {
      _bufferBytes = 1024;
      _buffer = static_cast<uint8_t *>(malloc(_bufferBytes));
    }
    if (!_buffer) { _bufferBytes = 0; return false; }
  }
  return true;
}

void HttpClient::end() {
  dropConnection();
  if (_buffer) {
    heap_caps_free(_buffer);
    _buffer = nullptr;
    _bufferBytes = 0;
  }
}

void HttpClient::closeIdle() { dropConnection(); }

void HttpClient::dropConnection() {
  if (_open) _open->stop();
  _plain.stop();
  _secure.stop();
  _open = nullptr;
  _openUrl = Url();
}

const char *HttpClient::methodName(Method method) {
  switch (method) {
    case Method::Get: return "GET";
    case Method::Post: return "POST";
    case Method::Put: return "PUT";
    case Method::Patch: return "PATCH";
    case Method::Delete: return "DELETE";
  }
  return "GET";
}

bool HttpClient::validHeaderName(const char *name, size_t length) {
  if (!name || !length || length > 64) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = name[i];
    const bool token = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                       strchr("!#$%&'*+-.^_`|~", c) != nullptr;
    if (!token || c == 0) return false;
  }
  return true;
}

bool HttpClient::validHeaderValue(const char *value, size_t length) {
  if (!value || length > 2048) return false;
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (c == '\r' || c == '\n' || c == 0 || (c < 0x20 && c != '\t') || c == 0x7f) return false;
  }
  return true;
}

bool HttpClient::reservedHeader(const String &name) {
  return name == "host" || name == "content-length" || name == "transfer-encoding" ||
         name == "connection" || name == "accept-encoding" || name == "te" ||
         name == "upgrade" || name == "keep-alive" || name == "trailer";
}

bool HttpClient::parseUrl(const String &value, Url &url) {
  url = Url();
  if (!value.length() || value.length() > MaxUrlBytes) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if (c <= 0x20 || c == 0x7f || c >= 0x80) return false;
  }
  const int schemeEnd = value.indexOf("://");
  if (schemeEnd <= 0) return false;
  url.scheme = lowercase(value.substring(0, schemeEnd));
  if (url.scheme != "http" && url.scheme != "https") return false;
  const int authorityStart = schemeEnd + 3;
  int authorityEnd = value.length();
  for (int i = authorityStart; i < static_cast<int>(value.length()); ++i) {
    const char c = value[i];
    if (c == '/' || c == '?' || c == '#') { authorityEnd = i; break; }
  }
  String authority = value.substring(authorityStart, authorityEnd);
  if (!authority.length() || authority.indexOf('@') >= 0 || authority.indexOf('[') >= 0) return false;
  url.port = url.scheme == "https" ? 443 : 80;
  const int colon = authority.indexOf(':');
  if (colon >= 0) {
    const String port = authority.substring(colon + 1);
    if (!port.length() || port.length() > 5) return false;
    for (size_t i = 0; i < port.length(); ++i) if (!isDigit(port[i])) return false;
    const long number = port.toInt();
    if (number < 1 || number > 65535) return false;
    url.port = static_cast<uint16_t>(number);
    authority = authority.substring(0, colon);
  }
  url.host = lowercase(authority);
  if (!url.host.length() || url.host.length() > 253) return false;
  for (size_t i = 0; i < url.host.length(); ++i) {
    const char c = url.host[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) return false;
  }
  String target = value.substring(authorityEnd);
  const int fragment = target.indexOf('#');
  if (fragment >= 0) target.remove(fragment);
  if (!target.length()) target = "/";
  else if (target[0] == '?') target = "/" + target;
  url.target = target;
  return true;
}

bool HttpClient::resolve(const Url &base, const String &reference, Url &url) {
  const String value = trimmed(reference);
  if (!value.length()) return false;
  if (value.indexOf("://") > 0) return parseUrl(value, url);
  if (value.startsWith("//")) return parseUrl(base.scheme + ":" + value, url);
  String target;
  if (value[0] == '/') {
    target = value;
  } else if (value[0] == '?') {
    const int query = base.target.indexOf('?');
    target = (query >= 0 ? base.target.substring(0, query) : base.target) + value;
  } else {
    const int query = base.target.indexOf('?');
    const String path = query >= 0 ? base.target.substring(0, query) : base.target;
    target = path.substring(0, path.lastIndexOf('/') + 1) + value;
  }
  Url origin = base;
  origin.target = target;
  return parseUrl(urlText(origin), url);
}

String HttpClient::urlText(const Url &url) {
  const bool defaultPort = (url.scheme == "http" && url.port == 80) ||
                           (url.scheme == "https" && url.port == 443);
  return url.scheme + "://" + url.host + (defaultPort ? String() : ":" + String(url.port)) + url.target;
}

bool HttpClient::sameOrigin(const Url &a, const Url &b) {
  return a.scheme == b.scheme && a.host == b.host && a.port == b.port;
}

bool HttpClient::perform(const Request &request, Policy &policy, Sink &sink, Response &response) {
  response = Response();
  if (!_buffer && !begin()) { response.error = "Network memory unavailable"; return false; }
  Url url;
  if (!parseUrl(request.url, url)) { response.error = "Invalid URL"; return false; }
  if (!policy.hostAllowed(url)) { response.error = "Host not allowed"; return false; }
  const Url origin = url;
  Method method = request.method;
  const uint8_t *body = request.body;
  size_t bodyLength = request.bodyLength;
  const uint32_t started = millis();
  const uint32_t deadline = started + request.timeoutMs;
  _idleTimeoutMs = request.idleTimeoutMs;
  for (uint8_t hop = 0; hop <= MaxRedirects; ++hop) {
    Exchange exchange;
    bool reused = false;
    String error;
    bool headReady = false;
    for (uint8_t attempt = 0; attempt < 2 && !headReady; ++attempt) {
      if (!connect(url, policy, deadline, error, reused)) {
        response.error = error;
        return false;
      }
      Request effective = request;
      // Credentials stay with the origin the app addressed.
      Header filtered[MaxRequestHeaders];
      uint8_t filteredCount = 0;
      for (uint8_t i = 0; i < request.headerCount && i < MaxRequestHeaders; ++i) {
        const String name = lowercase(request.headers[i].name);
        if (!sameOrigin(url, origin) && (name == "authorization" || name == "cookie")) continue;
        filtered[filteredCount++] = request.headers[i];
      }
      effective.headers = filtered;
      effective.headerCount = filteredCount;
      policy.progress(Sending, 0, -1);
      if (!sendRequest(*_open, url, effective, method, body, bodyLength)) {
        dropConnection();
        if (reused && !policy.cancelled()) continue;  // A stale keep-alive socket.
        response.error = policy.cancelled() ? "Cancelled" : "Cannot connect";
        return false;
      }
      headReady = readHead(*_open, policy, deadline, exchange, response, error);
      if (!headReady) {
        dropConnection();
        if (reused && error == "Connection closed" && !policy.cancelled()) continue;
        response.error = error;
        return false;
      }
    }
    if (!headReady) { response.error = "Cannot connect"; return false; }
    const bool redirect = exchange.status == 301 || exchange.status == 302 ||
                          exchange.status == 303 || exchange.status == 307 ||
                          exchange.status == 308;
    if (redirect && request.followRedirects && exchange.location.length()) {
      Url next;
      if (!resolve(url, exchange.location, next)) { dropConnection(); response.error = "Invalid redirect"; return false; }
      if (!policy.hostAllowed(next)) { dropConnection(); response.error = "Redirect not allowed"; return false; }
      log(String("redirect ") + exchange.status + " " + url.host + " -> " + next.host);
      if (exchange.status == 303 ||
          ((exchange.status == 301 || exchange.status == 302) && method != Method::Get)) {
        method = Method::Get;
        body = nullptr;
        bodyLength = 0;
      }
      dropConnection();
      url = next;
      continue;
    }
    if (exchange.encoded) {
      dropConnection();
      response.error = "Compressed response";
      return false;
    }
    response.status = exchange.status;
    response.finalUrl = urlText(url);
    const bool noBody = exchange.status < 200 || exchange.status == 204 || exchange.status == 304;
    const int32_t declared = noBody ? 0 : (exchange.chunked ? -1 : exchange.contentLength);
    if (!sink.begin(exchange.status, response.contentType, declared)) {
      dropConnection();
      response.error = "Response rejected";
      return false;
    }
    bool consumed = noBody;
    if (!noBody) {
      policy.progress(Receiving, 0, declared);
      if (!readBody(*_open, policy, deadline, exchange, sink, consumed, error)) {
        dropConnection();
        response.error = error;
        return false;
      }
    }
    if (!consumed || exchange.close) dropConnection();
    else _openUrl = url;
    if (!sink.finish()) { response.error = "Response rejected"; return false; }
    log(String(methodName(method)) + " " + url.host + " status=" + exchange.status +
        " reused=" + (reused ? "yes" : "no") + " ms=" + (millis() - started));
    return true;
  }
  dropConnection();
  response.error = "Too many redirects";
  return false;
}

bool HttpClient::connect(const Url &url, Policy &policy, uint32_t deadline,
                         String &error, bool &reused) {
  reused = false;
  if (_open && sameOrigin(_openUrl, url) && _open->connected()) {
    reused = true;
    return true;
  }
  dropConnection();
  if (policy.cancelled()) { error = "Cancelled"; return false; }
  if (WiFi.status() != WL_CONNECTED) { error = "Wi-Fi not connected"; return false; }
  const bool secure = url.scheme == "https";
  if (secure && time(nullptr) < static_cast<time_t>(ValidClockEpoch)) { error = "Clock not set"; return false; }
  policy.progress(Connecting, 0, -1);
  IPAddress address;
  if (WiFi.hostByName(url.host.c_str(), address) != 1) {
    error = "DNS lookup failed";
    log(String("DNS failed: host=") + url.host);
    return false;
  }
  const uint32_t budget = min(remainingMs(deadline), ConnectTimeoutMs);
  if (!budget) { error = "Timeout"; return false; }
  const uint32_t connectStarted = millis();
  bool connected = false;
  if (secure) {
    TlsMemoryScope tlsMemory(true);
    // stop() clears the SSL context, including the bundle hook, so attach the
    // linked root bundle before every handshake.
    _secure.setCACertBundle(x509CertificateBundleStart,
                            static_cast<size_t>(x509CertificateBundleEnd - x509CertificateBundleStart));
    _secure.setHandshakeTimeout(max<uint32_t>(1, budget / 1000));
    connected = _secure.connect(address, url.port, url.host.c_str(), nullptr, nullptr, nullptr);
    if (connected) _open = &_secure;
  } else {
    connected = _plain.connect(address, url.port, static_cast<int32_t>(budget));
    if (connected) _open = &_plain;
  }
  if (!connected) {
    if (policy.cancelled()) { error = "Cancelled"; return false; }
    if (secure) {
      char detail[96] = {};
      const int code = _secure.lastError(detail, sizeof(detail));
      _secure.stop();
      const bool tcp = _plain.connect(address, url.port, 1000);
      _plain.stop();
      error = tcp ? "TLS failed" : "Cannot connect";
      log(String("connect failed: host=") + url.host + " tls=" + code + " " + detail +
          " tcp=" + (tcp ? "ok" : "failed"));
    } else {
      error = remainingMs(deadline) ? "Cannot connect" : "Timeout";
      log(String("connect failed: host=") + url.host);
    }
    return false;
  }
  _openUrl = url;
  log(String("connected: host=") + url.host + " ms=" + (millis() - connectStarted) +
      " heap=" + ESP.getFreeHeap());
  return true;
}

bool HttpClient::sendRequest(WiFiClient &client, const Url &url, const Request &request,
                             Method method, const uint8_t *body, size_t bodyLength) {
  const bool defaultPort = (url.scheme == "http" && url.port == 80) ||
                           (url.scheme == "https" && url.port == 443);
  String head;
  head.reserve(256 + url.target.length());
  head += methodName(method);
  head += ' ';
  head += url.target;
  head += " HTTP/1.1\r\nHost: ";
  head += url.host;
  if (!defaultPort) head += ":" + String(url.port);
  bool userAgent = false;
  bool accept = false;
  for (uint8_t i = 0; i < request.headerCount; ++i) {
    const String name = lowercase(request.headers[i].name);
    if (name == "user-agent") userAgent = true;
    if (name == "accept") accept = true;
  }
  if (!userAgent) head += "\r\nUser-Agent: " + request.userAgent;
  if (!accept) head += "\r\nAccept: */*";
  head += "\r\nAccept-Encoding: identity\r\nConnection: keep-alive";
  for (uint8_t i = 0; i < request.headerCount; ++i) {
    head += "\r\n";
    head += request.headers[i].name;
    head += ": ";
    head += request.headers[i].value;
  }
  if (body || method == Method::Post || method == Method::Put || method == Method::Patch) {
    head += "\r\nContent-Length: " + String(static_cast<uint32_t>(bodyLength));
  }
  head += "\r\n\r\n";
  if (client.write(reinterpret_cast<const uint8_t *>(head.c_str()), head.length()) != head.length())
    return false;
  size_t sent = 0;
  while (sent < bodyLength) {
    const size_t chunk = min<size_t>(bodyLength - sent, 1024);
    const size_t written = client.write(body + sent, chunk);
    if (!written) return false;
    sent += written;
  }
  return true;
}

bool HttpClient::readLine(WiFiClient &client, Policy &policy, uint32_t deadline,
                          String &line, bool &tooLong) {
  line = "";
  tooLong = false;
  while (remainingMs(deadline)) {
    if (policy.cancelled()) return false;
    while (client.available()) {
      const int value = client.read();
      if (value < 0) break;
      if (value == '\n') {
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        return true;
      }
      if (line.length() >= MaxLineBytes) { tooLong = true; continue; }
      line += static_cast<char>(value);
    }
    if (!client.connected() && !client.available()) return false;
    delay(1);
  }
  return false;
}

int HttpClient::readSome(WiFiClient &client, Policy &policy, uint32_t deadline,
                         uint8_t *buffer, size_t capacity) {
  while (remainingMs(deadline)) {
    if (policy.cancelled()) return -1;
    const int available = client.available();
    if (available > 0) {
      const int received = client.read(buffer, min<size_t>(static_cast<size_t>(available), capacity));
      if (received > 0) return received;
    }
    if (!client.connected() && !client.available()) return 0;
    delay(1);
  }
  return -1;
}

bool HttpClient::readHead(WiFiClient &client, Policy &policy, uint32_t deadline,
                          Exchange &exchange, Response &response, String &error) {
  String line;
  bool tooLong = false;
  if (!readLine(client, policy, deadline, line, tooLong)) {
    error = policy.cancelled() ? "Cancelled" : remainingMs(deadline) ? "Connection closed" : "Timeout";
    return false;
  }
  if (!line.startsWith("HTTP/1.") || line.length() < 12) { error = "Invalid HTTP response"; return false; }
  exchange.status = line.substring(9, 12).toInt();
  exchange.close = line.startsWith("HTTP/1.0");
  if (exchange.status < 100 || exchange.status > 599) { error = "Invalid HTTP response"; return false; }
  response.headerCount = 0;
  response.headersTruncated = false;
  size_t headerBytes = 0;
  for (uint8_t count = 0; count < MaxHeaderLines; ++count) {
    if (!readLine(client, policy, deadline, line, tooLong)) {
      error = policy.cancelled() ? "Cancelled" : "Timeout";
      return false;
    }
    if (!line.length()) {
      if (exchange.status >= 100 && exchange.status < 200) {
        // Skip an interim response and read the final one.
        return readHead(client, policy, deadline, exchange, response, error);
      }
      return true;
    }
    if (tooLong) { response.headersTruncated = true; continue; }
    const int colon = line.indexOf(':');
    if (colon <= 0) continue;
    const String name = lowercase(trimmed(line.substring(0, colon)));
    const String value = trimmed(line.substring(colon + 1));
    if (name == "content-length") exchange.contentLength = value.toInt();
    else if (name == "transfer-encoding") exchange.chunked = lowercase(value).indexOf("chunked") >= 0;
    else if (name == "connection") exchange.close = exchange.close || lowercase(value).indexOf("close") >= 0;
    else if (name == "content-encoding") exchange.encoded = !value.equalsIgnoreCase("identity");
    else if (name == "location") exchange.location = value;
    if (name == "content-type") response.contentType = lowercase(value);
    if (response.headerCount < MaxResponseHeaders &&
        headerBytes + name.length() + value.length() <= MaxResponseHeaderBytes) {
      response.headers[response.headerCount++] = {name, value};
      headerBytes += name.length() + value.length();
    } else {
      response.headersTruncated = true;
    }
  }
  error = "Too many response headers";
  return false;
}

bool HttpClient::readBody(WiFiClient &client, Policy &policy, uint32_t deadline,
                          const Exchange &exchange, Sink &sink, bool &consumed, String &error) {
  consumed = false;
  uint32_t received = 0;
  auto failRead = [&](int result) {
    error = policy.cancelled() ? "Cancelled" : result < 0 ? "Timeout" : "Connection closed";
    return false;
  };
  auto deliver = [&](size_t amount) -> bool {
    received += amount;
    if (_idleTimeoutMs) deadline = millis() + _idleTimeoutMs;
    policy.progress(Receiving, received, exchange.chunked ? -1 : exchange.contentLength);
    if (!sink.write(_buffer, amount)) { error = "Response rejected"; return false; }
    return true;
  };
  if (exchange.chunked) {
    String line;
    bool tooLong = false;
    while (true) {
      if (!readLine(client, policy, deadline, line, tooLong) || tooLong) return failRead(-1);
      const int semicolon = line.indexOf(';');
      if (semicolon >= 0) line.remove(semicolon);
      line.trim();
      if (!line.length()) { error = "Invalid chunked response"; return false; }
      char *end = nullptr;
      const unsigned long size = strtoul(line.c_str(), &end, 16);
      if (!end || *end) { error = "Invalid chunked response"; return false; }
      if (size == 0) {
        do {
          if (!readLine(client, policy, deadline, line, tooLong)) break;
        } while (line.length());
        consumed = true;
        return true;
      }
      unsigned long left = size;
      while (left) {
        const int count = readSome(client, policy, deadline, _buffer, min<size_t>(left, _bufferBytes));
        if (count <= 0) return failRead(count);
        if (!deliver(static_cast<size_t>(count))) return false;
        left -= static_cast<unsigned long>(count);
      }
      if (!readLine(client, policy, deadline, line, tooLong) || line.length()) {
        error = "Invalid chunked response";
        return false;
      }
    }
  }
  if (exchange.contentLength >= 0) {
    uint32_t left = static_cast<uint32_t>(exchange.contentLength);
    while (left) {
      const int count = readSome(client, policy, deadline, _buffer, min<size_t>(left, _bufferBytes));
      if (count <= 0) return failRead(count);
      if (!deliver(static_cast<size_t>(count))) return false;
      left -= static_cast<uint32_t>(count);
    }
    consumed = true;
    return true;
  }
  // No framing: the body ends when the server closes the connection.
  while (true) {
    const int count = readSome(client, policy, deadline, _buffer, _bufferBytes);
    if (count == 0) return true;
    if (count < 0) return failRead(count);
    if (!deliver(static_cast<size_t>(count))) return false;
  }
}
