#include "BrowserTransport.h"

#include <WiFi.h>
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include <stdlib.h>

#include "BrowserDownloadStore.h"
#include "BrowserLimits.h"
#include "../../services/network/WiFiController.h"

namespace {
void browserLog(const __FlashStringHelper *message) {
  Serial.print(F("[Browser] "));
  Serial.println(message);
}

void browserLog(const String &message) {
  Serial.print(F("[Browser] "));
  Serial.println(message);
}

String clientState(WiFiClient &client) {
  return String(F("connected=")) + (client.connected() ? F("yes") : F("no")) +
         F(", available=") + String(client.available());
}

bool startsWithIgnoreCase(const String &value, const char *prefix) {
  const size_t length = strlen(prefix);
  return value.length() >= length && value.substring(0, length).equalsIgnoreCase(prefix);
}

class HttpLock {
public:
  explicit HttpLock(SemaphoreHandle_t lock) : _lock(lock), _locked(false) {}
  ~HttpLock() { if (_locked) xSemaphoreGive(_lock); }

  bool acquire(TickType_t timeout) {
    if (_locked) return true;
    _locked = _lock && xSemaphoreTake(_lock, timeout) == pdTRUE;
    return _locked;
  }

  bool locked() const { return _locked; }

private:
  SemaphoreHandle_t _lock;
  bool _locked;
};
}  // namespace

BrowserTransport::BrowserTransport(WiFiController &wifi, BrowserDownloadStore &downloadStore)
    : _wifi(wifi), _downloadStore(downloadStore), _mutex(xSemaphoreCreateMutex()) {}

bool BrowserTransport::discardDownload(const String &path) {
  return _downloadStore.discard(path);
}

void BrowserTransport::stopClients() {
  _plainClient.stop();
  _secureClient.stop();
}

Browser::TransferResult BrowserTransport::transfer(
    const Browser::HttpRequest &initialRequest,
    const Browser::RequestOptions &options,
    Browser::TransferObserver &observer,
    const Browser::CookieProvider &cookieProvider) {
  Browser::TransferResult result;
  result.finalUrl = initialRequest.url;
  if (initialRequest.method == Browser::HttpMethod::Post &&
      initialRequest.body.length() > BrowserLimits::MaxFormPostBytes) {
    result.error = "Form data is too large";
    return result;
  }
  if (!_wifi.isConnected()) {
    result.error = "Connect to Wi-Fi first";
    return result;
  }

  observer.phaseChanged(F("Waiting for network access"), -1);
  const uint32_t waitStartedMs = millis();
  HttpLock lock(_mutex);
  while (!lock.acquire(pdMS_TO_TICKS(50))) {
    if (observer.cancelled()) {
      result.error = "Loading cancelled";
      browserLog(String(F("network access wait cancelled: waitedMs=")) + String(millis() - waitStartedMs));
      return result;
    }
    if (millis() - waitStartedMs >= BrowserLimits::NetworkLockTimeoutMs) {
      browserLog(String(F("network access wait timed out: waitedMs=")) + String(millis() - waitStartedMs));
      result.error = "Browser is busy; try again";
      return result;
    }
    delay(1);
    yield();
  }
  if (!lock.locked()) {
    result.error = "Browser is busy";
    return result;
  }

  browserLog(F("network access acquired"));
  stopClients();
  _downloadStore.abort();
  const uint32_t bodyLimit = options.bodyLimit == 0 || options.bodyLimit > BrowserLimits::MaxBodyBytes
    ? BrowserLimits::MaxBodyBytes : options.bodyLimit;
  const uint32_t downloadLimit = options.downloadLimit == 0 ||
                                 options.downloadLimit > BrowserLimits::MaxDownloadBytes
    ? BrowserLimits::MaxDownloadBytes : options.downloadLimit;

  Browser::HttpRequest request = initialRequest;
  for (uint8_t redirect = 0; redirect <= BrowserLimits::MaxRedirects; ++redirect) {
    result.finalUrl = request.url;
    Browser::Url url;
    if (!parseUrl(request.url, url)) {
      browserLog(F("invalid redirect target"));
      result.error = "Invalid redirect URL";
      break;
    }
    browserLog(String(F("HTTP request ")) + String(redirect + 1) + F("/") +
               String(BrowserLimits::MaxRedirects + 1) + F(": method=") +
               (request.method == Browser::HttpMethod::Post ? F("POST") : F("GET")) +
               F(", url=") + request.url +
               (request.method == Browser::HttpMethod::Post
                  ? String(F(", bodyBytes=")) + String(request.body.length()) : String()));

    HttpResponse response{};
    WiFiClient *client = nullptr;
    const String cookieHeader = redirect == 0
      ? options.cookieHeader : cookieProvider.cookieHeaderFor(url);
    if (!requestOne(url, request.method, request.body, options.userAgent, cookieHeader,
                    response, client, observer, result.error)) {
      browserLog(String(F("HTTP request failed: url=")) + request.url + F(", error=") + result.error);
      break;
    }

    if (response.statusCode >= 300 && response.statusCode <= 399) {
      client->stop();
      if (!response.location.length()) {
        browserLog(String(F("redirect rejected: url=")) + request.url + F(", status=") +
                   String(response.statusCode) + F(", reason=no Location header"));
        result.error = "Redirect has no destination";
        break;
      }
      const String resolvedLocation = resolveUrl(url, response.location);
      browserLog(String(F("redirect received: status=")) + String(response.statusCode) +
                 F(", request=") + request.url + F(", Location=") + response.location +
                 F(", resolved=") + resolvedLocation + F(", returnsToInitial=") +
                 (resolvedLocation == initialRequest.url ? F("yes") : F("no")));
      if (response.statusCode == 301 || response.statusCode == 302 || response.statusCode == 303) {
        request.method = Browser::HttpMethod::Get;
        request.body = "";
      }
      request.url = resolvedLocation;
      continue;
    }

    if (response.statusCode == 401) {
      client->stop();
      browserLog(String(F("server requires unsupported Basic authentication: url=")) + request.url);
      result.error = "Basic authentication is unsupported";
      break;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
      client->stop();
      browserLog(String(F("HTTP request rejected: url=")) + request.url + F(", status=") +
                 String(response.statusCode));
      result.error = "HTTP error " + String(response.statusCode);
      break;
    }

    result.contentType = response.contentType;
    const FileFormats::Format format = BrowserDownloadStore::classify(response.contentType, url.path);
    const bool downloadable = FileFormats::isDownloadable(format);
    browserLog(String(F("response accepted: type=")) +
               (response.contentType.length() ? response.contentType : String(F("unspecified"))) +
               F(", length=") + String(response.contentLength) +
               (response.chunked ? F(", chunked") : F("")) +
               (downloadable ? F(", media download") : F(", page response")));

    bool readOk = false;
    if (downloadable && response.compressed) {
      browserLog(String(F("compressed media download rejected: url=")) + request.url +
                 F(", encoding is not identity"));
      result.error = "Compressed file response unsupported";
    } else if (downloadable) {
      readOk = readDownloadBody(
        *client, response.chunked, response.contentLength, format,
        BrowserDownloadStore::deriveSuggestedName(response.contentDisposition, url.path, format),
        downloadLimit, result.downloadPath, observer, result.error);
    } else {
      readOk = readBody(*client, response.chunked, response.contentLength, bodyLimit,
                        result.retainedText, observer, result.error);
    }
    if (!readOk) {
      browserLog(String(F("response transfer failed: url=")) + request.url + F(", error=") +
                 result.error + F(", retainedBytes=") + String(result.retainedText.length()) +
                 F(", ") + clientState(*client));
      client->stop();
      break;
    }

    client->stop();
    String canonicalUrl;
    String canonicalError;
    if (normalizeUrl(request.url, canonicalUrl, canonicalError)) result.finalUrl = canonicalUrl;
    result.error = "";
    browserLog(String(F("HTTP transfer complete: ")) + result.finalUrl);
    stopClients();
    _downloadStore.abort();
    return result;
  }

  if (!result.error.length()) {
    browserLog(String(F("redirect limit reached: initialUrl=")) + initialRequest.url +
               F(", limit=") + String(BrowserLimits::MaxRedirects));
    result.error = "Too many redirects";
  }
  result.downloadPath = "";
  stopClients();
  _downloadStore.abort();
  return result;
}

bool BrowserTransport::requestOne(const Browser::Url &url, Browser::HttpMethod method,
                                  const String &requestBody, const String &userAgent,
                                  const String &cookieHeader, HttpResponse &response,
                                  WiFiClient *&client, Browser::TransferObserver &observer,
                                  String &error) {
  if (observer.cancelled()) {
    error = "Loading cancelled";
    browserLog(String(F("request cancelled before connect: url=")) + url.scheme +
               F("://") + url.host + url.path);
    return false;
  }

  stopClients();
  client = &_plainClient;
  if (url.scheme == "https") {
    _secureClient.setInsecure();
    _secureClient.setHandshakeTimeout(BrowserLimits::ConnectTimeoutMs / 1000);
    client = &_secureClient;
  }

  observer.phaseChanged(String(F("Connecting to ")) + url.host, -1);
  browserLog(String(F("connecting: ")) + url.scheme + F("://") + url.host + F(":") + String(url.port));
  IPAddress resolvedAddress;
  const int dnsResult = WiFi.hostByName(url.host.c_str(), resolvedAddress);
  if (dnsResult != 1) {
    browserLog(String(F("DNS lookup failed: host=")) + url.host + F(", result=") +
               String(dnsResult) + F(", wifiStatus=") + String((int)WiFi.status()) +
               F(", rssi=") + String(WiFi.RSSI()));
    error = "DNS lookup failed";
    return false;
  }
  browserLog(String(F("DNS resolved: host=")) + url.host + F(", address=") + resolvedAddress.toString());
  browserLog(String(F("TLS memory before connect: freeHeap=")) + String(ESP.getFreeHeap()) +
             F(", largestHeapBlock=") + String(ESP.getMaxAllocHeap()) + F(", freePsram=") +
             String(ESP.getFreePsram()) + F(", largestPsramBlock=") + String(ESP.getMaxAllocPsram()));

  const uint32_t initialConnectStartedMs = millis();
  bool connected = client->connect(url.host.c_str(), url.port,
                                   (int32_t)BrowserLimits::ConnectTimeoutMs);
  const uint32_t initialConnectElapsedMs = millis() - initialConnectStartedMs;
  String rawTcpState = "not-tested";
  uint32_t retryConnectElapsedMs = 0;
  if (!connected && url.scheme == "https") {
    _plainClient.stop();
    rawTcpState = _plainClient.connect(resolvedAddress, url.port, 1000) ? "connected" : "failed";
    _plainClient.stop();
    if (rawTcpState == "connected") {
      _secureClient.stop();
      _secureClient.setHandshakeTimeout(BrowserLimits::TlsRetryHandshakeTimeoutSeconds);
      browserLog(String(F("HTTPS retrying resolved address: host=")) + url.host + F(", address=") +
                 resolvedAddress.toString() + F(", handshakeTimeoutSeconds=") +
                 String(BrowserLimits::TlsRetryHandshakeTimeoutSeconds));
      const uint32_t retryConnectStartedMs = millis();
      connected = _secureClient.connect(resolvedAddress, url.port, url.host.c_str(),
                                        nullptr, nullptr, nullptr);
      retryConnectElapsedMs = millis() - retryConnectStartedMs;
      if (connected) {
        browserLog(String(F("HTTPS retry via resolved address succeeded: elapsedMs=")) +
                   String(retryConnectElapsedMs));
      }
    }
  }
  if (!connected) {
    char tlsErrorText[96] = {};
    const int tlsError = url.scheme == "https"
      ? _secureClient.lastError(tlsErrorText, sizeof(tlsErrorText)) : 0;
    browserLog(String(F("connection failed: host=")) + url.host + F(", port=") + String(url.port) +
               F(", initialConnectMs=") + String(initialConnectElapsedMs) + F(", timeoutMs=") +
               String(BrowserLimits::ConnectTimeoutMs) + F(", wifiStatus=") +
               String((int)WiFi.status()) + F(", rssi=") + String(WiFi.RSSI()) + F(", tlsError=") +
               String(tlsError) + F(", tlsDetail=") + tlsErrorText + F(", retryConnectMs=") +
               String(retryConnectElapsedMs) + F(", rawTcp=") + rawTcpState + F(", ") +
               clientState(*client));
    error = observer.cancelled() ? "Loading cancelled" : "Connection failed";
    return false;
  }

  observer.phaseChanged(F("Sending request"), -1);
  if (observer.cancelled()) {
    client->stop();
    error = "Loading cancelled";
    return false;
  }

  String hostHeader = url.host;
  const bool defaultPort = (url.scheme == "http" && url.port == 80) ||
                           (url.scheme == "https" && url.port == 443);
  if (!defaultPort) hostHeader += ":" + String(url.port);
  const bool post = method == Browser::HttpMethod::Post;
  String request = String(post ? "POST" : "GET") + " " + url.path +
                   " HTTP/1.1\r\nHost: " + hostHeader + "\r\nUser-Agent: " + userAgent +
                   "\r\nAccept: text/html,*/*;q=0.1\r\nAccept-Encoding: identity\r\nConnection: close\r\n";
  if (post) {
    request += "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: " +
               String(requestBody.length()) + "\r\n";
  }
  if (cookieHeader.length()) request += "Cookie: " + cookieHeader + "\r\n";
  request += "\r\n";

  const size_t headerBytes = client->print(request);
  if (headerBytes != request.length()) {
    browserLog(String(F("request header write failed: host=")) + url.host + F(", sent=") +
               String(headerBytes) + F(", expected=") + String(request.length()) + F(", ") +
               clientState(*client));
    client->stop();
    error = "Request failed";
    return false;
  }
  if (post && requestBody.length()) {
    if (observer.cancelled()) {
      client->stop();
      error = "Loading cancelled";
      return false;
    }
    const size_t bodyBytes = client->write(
      reinterpret_cast<const uint8_t *>(requestBody.c_str()), requestBody.length());
    if (bodyBytes != requestBody.length()) {
      browserLog(String(F("request body write failed: host=")) + url.host + F(", sent=") +
                 String(bodyBytes) + F(", expected=") + String(requestBody.length()) + F(", ") +
                 clientState(*client));
      client->stop();
      error = "Request failed";
      return false;
    }
  }
  browserLog(String(F("request sent: method=")) + (post ? F("POST") : F("GET")) +
             F(", host=") + url.host + F(", bodyBytes=") + String(post ? requestBody.length() : 0));

  String line;
  observer.phaseChanged(F("Waiting for response"), -1);
  if (!readLine(*client, line, BrowserLimits::ConnectTimeoutMs, observer) ||
      !startsWithIgnoreCase(line, "HTTP/")) {
    browserLog(String(F("invalid or timed out HTTP status line: host=")) + url.host +
               F(", bytes=") + String(line.length()) + F(", timeoutMs=") +
               String(BrowserLimits::ConnectTimeoutMs) + F(", ") + clientState(*client));
    client->stop();
    error = "Invalid HTTP response";
    return false;
  }
  const int firstSpace = line.indexOf(' ');
  response.statusCode = firstSpace >= 0 ? line.substring(firstSpace + 1).toInt() : 0;
  if (response.statusCode == 0) {
    browserLog(String(F("invalid HTTP status: host=")) + url.host + F(", line=") + line);
    client->stop();
    client = nullptr;
    error = "Invalid HTTP status";
    return false;
  }
  browserLog(String(F("HTTP status: ")) + String(response.statusCode));

  response.chunked = false;
  response.compressed = false;
  response.contentLength = -1;
  response.contentType = "";
  response.contentDisposition = "";
  response.location = "";
  bool headersComplete = false;
  for (uint8_t count = 0; count < BrowserLimits::MaxResponseHeaderLines; ++count) {
    bool lineTruncated = false;
    if (!readLine(*client, line, BrowserLimits::ConnectTimeoutMs, observer, true, &lineTruncated)) {
      browserLog(String(F("response header read failed: host=")) + url.host + F(", headerCount=") +
                 String(count) + F(", retainedLineBytes=") + String(line.length()) + F(", timeoutMs=") +
                 String(BrowserLimits::ConnectTimeoutMs) + F(", ") + clientState(*client));
      client->stop();
      error = "Response headers timed out";
      return false;
    }
    if (lineTruncated) {
      browserLog(String(F("discarded oversized response header: host=")) + url.host +
                 F(", headerCount=") + String(count) + F(", retainedBytes=") +
                 String(line.length()) + F(", maxBytes=") + String(BrowserLimits::MaxHttpLineBytes));
      continue;
    }
    if (!line.length()) {
      headersComplete = true;
      break;
    }
    const int colon = line.indexOf(':');
    if (colon <= 0) continue;
    const String name = lower(trimCopy(line.substring(0, colon)));
    const String headerValue = trimCopy(line.substring(colon + 1));
    if (name == "content-length") response.contentLength = headerValue.toInt();
    else if (name == "transfer-encoding" && lower(headerValue).indexOf("chunked") >= 0) response.chunked = true;
    else if (name == "content-encoding" && !headerValue.equalsIgnoreCase("identity")) response.compressed = true;
    else if (name == "content-type") response.contentType = lower(headerValue);
    else if (name == "content-disposition") response.contentDisposition = headerValue;
    else if (name == "location") response.location = headerValue;
    else if (name == "set-cookie") observer.cookieReceived(headerValue, url);
  }
  if (!headersComplete) {
    browserLog(String(F("too many response headers: host=")) + url.host + F(", limit=") +
               String(BrowserLimits::MaxResponseHeaderLines) + F(", ") + clientState(*client));
    client->stop();
    error = "Too many response headers";
    return false;
  }
  browserLog(String(F("headers complete: length=")) + String(response.contentLength) +
             F(", type=") + (response.contentType.length() ? response.contentType : String(F("unspecified"))) +
             (response.contentDisposition.length()
                ? String(F(", disposition=")) + response.contentDisposition : String()) +
             (response.chunked ? F(", chunked") : F("")) +
             (response.compressed ? F(", compressed") : F("")));
  return true;
}

bool BrowserTransport::readLine(WiFiClient &client, String &line, uint32_t timeoutMs,
                                Browser::TransferObserver &observer, bool discardLongLine,
                                bool *lineTruncated) {
  line = "";
  if (lineTruncated) *lineTruncated = false;
  bool truncated = false;
  const uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (observer.cancelled()) return false;
    while (client.available()) {
      const int value = client.read();
      if (value < 0) continue;
      if (value == '\n') {
        if (!truncated && line.endsWith("\r")) line.remove(line.length() - 1);
        if (lineTruncated) *lineTruncated = truncated;
        return true;
      }
      if (truncated) continue;
      if (line.length() >= BrowserLimits::MaxHttpLineBytes) {
        if (!discardLongLine) return false;
        truncated = true;
        continue;
      }
      line += (char)value;
    }
    if (!client.connected() && !client.available()) {
      if (lineTruncated) *lineTruncated = truncated;
      return line.length() > 0;
    }
    delay(1);
    yield();
  }
  return false;
}

int BrowserTransport::readByte(WiFiClient &client, uint32_t timeoutMs,
                               Browser::TransferObserver &observer) {
  const uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (observer.cancelled()) return -1;
    if (client.available()) return client.read();
    if (!client.connected()) return -1;
    delay(1);
    yield();
  }
  return -1;
}

bool BrowserTransport::readBody(WiFiClient &client, bool chunked, int32_t contentLength,
                                uint32_t bodyLimit, String &body,
                                Browser::TransferObserver &observer, String &error) {
  observer.phaseChanged(F("Loading page"), chunked ? -1 : contentLength);
  browserLog(String(F("reading page body")) + (chunked ? F(" (chunked)") : F("")) +
             F(", expected bytes=") + String(contentLength));
  body = "";
  const uint32_t reserveBytes = contentLength > 0
    ? (contentLength < (int32_t)bodyLimit ? (uint32_t)contentLength : bodyLimit) : 4096;
  if (!body.reserve(reserveBytes)) {
    browserLog(String(F("page body allocation failed: requestedBytes=")) + String(reserveBytes) +
               F(", freeHeap=") + String(ESP.getFreeHeap()) + F(", largestBlock=") +
               String(ESP.getMaxAllocHeap()));
    error = "Not enough memory for page";
    return false;
  }

  uint32_t total = 0;
  uint32_t reported = 0;
  auto reportProgress = [&]() {
    if (total - reported >= 1024) {
      observer.bytesReceived(total - reported);
      reported = total;
    }
  };
  auto finishProgress = [&]() {
    if (total > reported) observer.bytesReceived(total - reported);
  };

  if (chunked) {
    while (true) {
      String sizeLine;
      if (!readLine(client, sizeLine, BrowserLimits::ConnectTimeoutMs, observer)) {
        error = "Page body timed out";
        return false;
      }
      const int semicolon = sizeLine.indexOf(';');
      if (semicolon >= 0) sizeLine.remove(semicolon);
      const uint32_t chunk = strtoul(sizeLine.c_str(), nullptr, 16);
      if (chunk == 0) {
        do {
          if (!readLine(client, sizeLine, BrowserLimits::ConnectTimeoutMs, observer)) break;
        } while (sizeLine.length());
        finishProgress();
        browserLog(String(F("page body complete: bytes=")) + String(total));
        return true;
      }
      if (chunk > bodyLimit - total) {
        const uint32_t keep = bodyLimit - total;
        for (uint32_t index = 0; index < keep; ++index) {
          const int value = readByte(client, BrowserLimits::ConnectTimeoutMs, observer);
          if (value < 0) {
            error = "Page body timed out";
            return false;
          }
          body += (char)value;
          ++total;
          reportProgress();
        }
        finishProgress();
        browserLog(String(F("page body reached retained limit: bytes=")) + String(total));
        return true;
      }
      for (uint32_t index = 0; index < chunk; ++index) {
        const int value = readByte(client, BrowserLimits::ConnectTimeoutMs, observer);
        if (value < 0) {
          error = "Page body timed out";
          return false;
        }
        body += (char)value;
        ++total;
        reportProgress();
      }
      const int cr = readByte(client, BrowserLimits::ConnectTimeoutMs, observer);
      const int lf = readByte(client, BrowserLimits::ConnectTimeoutMs, observer);
      if (cr != '\r' || lf != '\n') {
        error = "Invalid chunked response";
        return false;
      }
    }
  }

  while (contentLength < 0 || total < (uint32_t)contentLength) {
    const int value = readByte(client, BrowserLimits::ConnectTimeoutMs, observer);
    if (value < 0) {
      if (contentLength < 0) {
        finishProgress();
        browserLog(String(F("page body complete at connection close: bytes=")) + String(total));
        return true;
      }
      error = contentLength < 0 ? "Page body timed out" : "Incomplete page body";
      return false;
    }
    if (total >= bodyLimit) {
      finishProgress();
      browserLog(String(F("page body reached retained limit: bytes=")) + String(total));
      return true;
    }
    body += (char)value;
    ++total;
    reportProgress();
  }
  finishProgress();
  browserLog(String(F("page body complete: bytes=")) + String(total));
  return true;
}

bool BrowserTransport::readDownloadBody(WiFiClient &client, bool chunked,
                                        int32_t contentLength, FileFormats::Format format,
                                        const String &suggestedFileName, uint32_t downloadLimit,
                                        String &downloadedPath,
                                        Browser::TransferObserver &observer, String &error) {
  downloadedPath = "";
  observer.phaseChanged(String(F("Downloading ")) + FileFormats::extension(format),
                        chunked ? -1 : contentLength);
  browserLog(String(F("starting file download: format=")) + FileFormats::extension(format) +
             F(", expected bytes=") + String(contentLength) +
             (chunked ? F(", chunked") : F("")));
  if (!_downloadStore.begin(format, suggestedFileName, contentLength, downloadLimit, error)) {
    browserLog(String(F("download storage begin failed: ")) + error);
    return false;
  }

  uint32_t total = 0;
  auto writeBytes = [&](const uint8_t *bytes, uint16_t length) -> bool {
    if (!_downloadStore.write(bytes, length, error)) return false;
    total += length;
    observer.bytesReceived(length);
    return true;
  };
  auto receiveBytes = [&](uint32_t length) -> bool {
    uint8_t buffer[512];
    while (length) {
      const uint16_t amount = length > sizeof(buffer) ? sizeof(buffer) : (uint16_t)length;
      for (uint16_t index = 0; index < amount; ++index) {
        const int value = readByte(client, BrowserLimits::ConnectTimeoutMs, observer);
        if (value < 0) {
          error = "File download timed out";
          return false;
        }
        buffer[index] = (uint8_t)value;
      }
      if (!writeBytes(buffer, amount)) return false;
      length -= amount;
    }
    return true;
  };

  bool complete = true;
  if (chunked) {
    while (complete) {
      String sizeLine;
      if (!readLine(client, sizeLine, BrowserLimits::ConnectTimeoutMs, observer)) {
        error = "File download timed out";
        complete = false;
        break;
      }
      const int semicolon = sizeLine.indexOf(';');
      if (semicolon >= 0) sizeLine.remove(semicolon);
      char *end = nullptr;
      const uint32_t amount = strtoul(sizeLine.c_str(), &end, 16);
      if (!sizeLine.length() || !end || *end != '\0') {
        error = "Invalid chunked download";
        complete = false;
        break;
      }
      if (amount == 0) {
        do {
          if (!readLine(client, sizeLine, BrowserLimits::ConnectTimeoutMs, observer)) {
            error = "File download timed out";
            complete = false;
            break;
          }
        } while (sizeLine.length());
        break;
      }
      if (!receiveBytes(amount)) {
        if (!error.length()) error = "File exceeds download limit";
        complete = false;
        break;
      }
      const int cr = readByte(client, BrowserLimits::ConnectTimeoutMs, observer);
      const int lf = readByte(client, BrowserLimits::ConnectTimeoutMs, observer);
      if (cr != '\r' || lf != '\n') {
        error = "Invalid chunked download";
        complete = false;
      }
    }
  } else if (contentLength >= 0) {
    complete = receiveBytes((uint32_t)contentLength);
    if (!complete && !error.length()) error = "Incomplete file download";
  } else {
    uint8_t buffer[512];
    uint16_t buffered = 0;
    while (true) {
      const int value = readByte(client, BrowserLimits::ConnectTimeoutMs, observer);
      if (value < 0) {
        if (!client.connected() && !client.available() && !observer.cancelled()) break;
        error = "File download timed out";
        complete = false;
        break;
      }
      buffer[buffered++] = (uint8_t)value;
      if (buffered == sizeof(buffer)) {
        if (!writeBytes(buffer, buffered)) {
          complete = false;
          break;
        }
        buffered = 0;
      }
    }
    if (complete && buffered && !writeBytes(buffer, buffered)) complete = false;
  }

  if (!complete) {
    browserLog(String(F("download failed after bytes=")) + String(total) + F(": ") + error);
    _downloadStore.abort();
    return false;
  }
  if (!_downloadStore.commit(downloadedPath, error)) {
    if (error == F("Unsupported file download")) {
      browserLog(F("download rejected: file signature does not match response format"));
    } else {
      browserLog(String(F("download commit failed: ")) + error);
    }
    _downloadStore.abort();
    return false;
  }
  if (!downloadedPath.length()) {
    browserLog(F("download rejected: file signature does not match response format"));
    error = "Unsupported file download";
    return false;
  }
  browserLog(String(F("download saved: bytes=")) + String(total) + F(", path=") + downloadedPath);
  return true;
}

bool BrowserTransport::normalizeUrl(const String &value, String &normalized, String &error) {
  normalized = trimCopy(value);
  error = "";
  if (!normalized.length()) {
    error = "Enter a URL";
    return false;
  }
  if (normalized.indexOf("://") < 0) normalized = "https://" + normalized;
  Browser::Url url{};
  if (!parseUrl(normalized, url)) {
    error = "Only valid HTTP/HTTPS URLs work";
    return false;
  }
  normalized = url.scheme + "://" + url.host;
  const bool defaultPort = (url.scheme == "http" && url.port == 80) ||
                           (url.scheme == "https" && url.port == 443);
  if (!defaultPort) normalized += ":" + String(url.port);
  normalized += url.path;
  return true;
}

bool BrowserTransport::parseUrl(const String &value, Browser::Url &url) {
  const int schemeEnd = value.indexOf("://");
  if (schemeEnd <= 0) return false;
  url.scheme = lower(value.substring(0, schemeEnd));
  if (url.scheme != "http" && url.scheme != "https") return false;
  const int authorityStart = schemeEnd + 3;
  int pathStart = value.indexOf('/', authorityStart);
  const int queryStart = value.indexOf('?', authorityStart);
  if (pathStart < 0 || (queryStart >= 0 && queryStart < pathStart)) pathStart = queryStart;
  String authority = pathStart < 0 ? value.substring(authorityStart)
                                   : value.substring(authorityStart, pathStart);
  if (!authority.length() || authority.indexOf('@') >= 0) return false;
  const int colon = authority.lastIndexOf(':');
  url.port = url.scheme == "https" ? 443 : 80;
  url.host = authority;
  if (colon >= 0) {
    const String portText = authority.substring(colon + 1);
    const long port = portText.toInt();
    if (port <= 0 || port > 65535 || String(port) != portText) return false;
    url.port = (uint16_t)port;
    url.host = authority.substring(0, colon);
  }
  url.host.trim();
  if (!url.host.length() || url.host.indexOf(' ') >= 0 || url.host.indexOf('[') >= 0) return false;
  url.host = lower(url.host);
  url.path = pathStart < 0 ? "/" : value.substring(pathStart);
  if (url.path.startsWith("?")) url.path = "/" + url.path;
  const int fragment = url.path.indexOf('#');
  if (fragment >= 0) url.path.remove(fragment);
  if (!url.path.length()) url.path = "/";
  return true;
}

String BrowserTransport::resolveUrl(const Browser::Url &base, const String &reference) {
  String target = trimCopy(reference);
  String root = base.scheme + "://" + base.host;
  const bool defaultPort = (base.scheme == "http" && base.port == 80) ||
                           (base.scheme == "https" && base.port == 443);
  if (!defaultPort) root += ":" + String(base.port);
  const int fragment = target.indexOf('#');
  if (fragment >= 0) target.remove(fragment);
  if (!target.length()) return root + base.path;
  if (target.startsWith("//")) return base.scheme + ":" + target;
  if (target.indexOf("://") >= 0) return target;
  const int colon = target.indexOf(':');
  int separator = target.length();
  int slash = target.indexOf('/');
  const int query = target.indexOf('?');
  if (slash >= 0 && slash < separator) separator = slash;
  if (query >= 0 && query < separator) separator = query;
  if (colon >= 0 && colon < separator) return target;
  if (target.startsWith("/")) return root + target;
  String path = base.path;
  const int queryStart = path.indexOf('?');
  if (queryStart >= 0) path.remove(queryStart);
  if (target.startsWith("?")) return root + path + target;
  String directory = path;
  slash = directory.lastIndexOf('/');
  directory = slash >= 0 ? directory.substring(0, slash + 1) : "/";
  return root + directory + target;
}

String BrowserTransport::trimCopy(const String &value) {
  String result = value;
  result.trim();
  return result;
}

String BrowserTransport::lower(const String &value) {
  String result = value;
  result.toLowerCase();
  return result;
}
