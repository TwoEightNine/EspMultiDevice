#pragma once

#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <freertos/semphr.h>

#include "BrowserTypes.h"

class WiFiController;

// Owns connection resources shared by foreground navigation and preview work.
// Transfer policy remains deliberately bounded and TLS remains compatibility
// mode (certificate validation disabled by the request implementation).
class BrowserTransport {
public:
  explicit BrowserTransport(WiFiController &wifi);

  WiFiClient &plainClient() { return _plainClient; }
  WiFiClientSecure &secureClient() { return _secureClient; }
  SemaphoreHandle_t mutex() const { return _mutex; }
  WiFiController &wifi() const { return _wifi; }
  void stopClients();

  static bool normalizeUrl(const String &value, String &normalized, String &error);
  static bool parseUrl(const String &value, Browser::Url &url);
  static String resolveUrl(const Browser::Url &base, const String &reference);

private:
  WiFiController &_wifi;
  WiFiClient _plainClient;
  WiFiClientSecure _secureClient;
  SemaphoreHandle_t _mutex;

  static String trimCopy(const String &value);
  static String lower(const String &value);
};
