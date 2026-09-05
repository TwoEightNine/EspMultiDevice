#pragma once

#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <freertos/semphr.h>

#include "../../media/FileFormats.h"
#include "BrowserTypes.h"

class BrowserDownloadStore;
class WiFiController;

namespace Browser {

struct RequestOptions {
  String userAgent;
  String cookieHeader;
  uint32_t bodyLimit;
  uint32_t downloadLimit;
};

class TransferObserver : public CancellationCheck {
public:
  virtual ~TransferObserver() = default;
  virtual void phaseChanged(const String &phase, int32_t totalBytes) = 0;
  virtual void bytesReceived(uint32_t bytes) = 0;
  virtual void cookieReceived(const String &header, const Url &url) = 0;
};

class CookieProvider {
public:
  virtual ~CookieProvider() = default;
  virtual String cookieHeaderFor(const Url &url) const = 0;
};

struct TransferResult {
  String finalUrl;
  String contentType;
  String retainedText;
  String downloadPath;
  String error;

  bool succeeded() const { return error.length() == 0; }
};

}  // namespace Browser

// Owns the Browser's complete synchronous HTTP transfer boundary. Callers
// provide transfer policy and observe progress without gaining access to the
// clients, serialization mutex, or download transaction.
class BrowserTransport {
public:
  BrowserTransport(WiFiController &wifi, BrowserDownloadStore &downloadStore);

  Browser::TransferResult transfer(const Browser::HttpRequest &initialRequest,
                                   const Browser::RequestOptions &options,
                                   Browser::TransferObserver &observer,
                                   const Browser::CookieProvider &cookieProvider);
  bool discardDownload(const String &path);

  static bool normalizeUrl(const String &value, String &normalized, String &error);
  static bool parseUrl(const String &value, Browser::Url &url);
  static String resolveUrl(const Browser::Url &base, const String &reference);

private:
  struct HttpResponse {
    int statusCode;
    String location;
    String contentType;
    String contentDisposition;
    bool chunked;
    bool compressed;
    int32_t contentLength;
  };

  WiFiController &_wifi;
  BrowserDownloadStore &_downloadStore;
  WiFiClient _plainClient;
  WiFiClientSecure _secureClient;
  SemaphoreHandle_t _mutex;

  void stopClients();
  bool requestOne(const Browser::Url &url, Browser::HttpMethod method,
                  const String &requestBody, const String &userAgent,
                  const String &cookieHeader, HttpResponse &response,
                  WiFiClient *&client, Browser::TransferObserver &observer,
                  String &error);
  bool readLine(WiFiClient &client, String &line, uint32_t timeoutMs,
                Browser::TransferObserver &observer, bool discardLongLine = false,
                bool *lineTruncated = nullptr);
  int readByte(WiFiClient &client, uint32_t timeoutMs,
               Browser::TransferObserver &observer);
  bool readBody(WiFiClient &client, bool chunked, int32_t contentLength,
                uint32_t bodyLimit, String &body,
                Browser::TransferObserver &observer, String &error);
  bool readDownloadBody(WiFiClient &client, bool chunked, int32_t contentLength,
                        FileFormats::Format format, const String &suggestedFileName,
                        uint32_t downloadLimit, String &downloadedPath,
                        Browser::TransferObserver &observer, String &error);

  static String trimCopy(const String &value);
  static String lower(const String &value);
};
