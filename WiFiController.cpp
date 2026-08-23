#include "WiFiController.h"

#include <esp_wifi.h>

WiFiController::WiFiController()
  : _enabled(false),
    _scanInProgress(false),
    _reconnectAllowed(true),
    _connectionStartedMs(0),
    _state(WiFiConnectionState::Disabled),
    _ssid(),
    _networkType("Wi-Fi"),
    _error(),
    _networkCount(0) {
}

void WiFiController::begin() {
  // Keep the radio off during display/storage startup; the splash screen
  // enables it after Home has finished its first render.
  _enabled = false;
  _reconnectAllowed = false;
  _scanInProgress = false;
  _error = "";
  _state = WiFiConnectionState::Disabled;
  WiFi.mode(WIFI_OFF);
}

void WiFiController::update(uint32_t nowMs) {
  if (!_enabled) {
    return;
  }

  if (_scanInProgress) {
    int16_t result = WiFi.scanComplete();
    if (result != WIFI_SCAN_RUNNING) {
      finishScan(result);
    }
  }

  if (WiFi.status() == WL_CONNECTED) {
    if (_state != WiFiConnectionState::Connected) {
      setConnectedState();
    }
    return;
  }

  if (_state == WiFiConnectionState::Connected && _reconnectAllowed && _ssid.length() > 0) {
    _state = WiFiConnectionState::Connecting;
    _connectionStartedMs = nowMs;
    WiFi.begin();
  }

  if (_state == WiFiConnectionState::Connecting &&
      nowMs - _connectionStartedMs >= ConnectionTimeoutMs) {
    setFailedState("Connection failed");
  }
}

void WiFiController::setEnabled(bool enabled, bool scanWhenDisconnected) {
  if (_enabled == enabled) {
    return;
  }

  _enabled = enabled;
  _error = "";
  _reconnectAllowed = enabled;
  if (!enabled) {
    _scanInProgress = false;
    WiFi.scanDelete();
    WiFi.disconnect(false, false);
    WiFi.mode(WIFI_OFF);
    _state = WiFiConnectionState::Disabled;
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  if (!reconnectToSavedNetwork(millis())) {
    _state = WiFiConnectionState::Disconnected;
    if (scanWhenDisconnected) {
      startScan();
    }
  }
}

bool WiFiController::isEnabled() const {
  return _enabled;
}

bool WiFiController::isConnected() const {
  return _enabled && _state == WiFiConnectionState::Connected &&
         WiFi.status() == WL_CONNECTED;
}

bool WiFiController::isScanning() const {
  return _scanInProgress;
}

WiFiConnectionState WiFiController::state() const {
  return _state;
}

const String &WiFiController::ssid() const {
  return _ssid;
}

const String &WiFiController::networkType() const {
  return _networkType;
}

const String &WiFiController::error() const {
  return _error;
}

bool WiFiController::connect(const String &ssid, const String &password) {
  if (!_enabled || ssid.length() == 0) {
    return false;
  }

  _ssid = ssid;
  _error = "";
  _reconnectAllowed = true;
  _state = WiFiConnectionState::Connecting;
  _connectionStartedMs = millis();

  for (uint8_t i = 0; i < _networkCount; i++) {
    if (_networks[i].ssid == ssid) {
      _networkType = securityType(_networks[i].authMode);
      break;
    }
  }

  WiFi.mode(WIFI_STA);
  // Keep credentials in the ESP32 Wi-Fi configuration so they survive reboot.
  WiFi.persistent(true);
  if (password.length() == 0) {
    WiFi.begin(ssid.c_str(), nullptr);
  } else {
    WiFi.begin(ssid, password);
  }
  return true;
}

void WiFiController::disconnect() {
  _reconnectAllowed = false;
  _scanInProgress = false;
  WiFi.scanDelete();
  WiFi.disconnect(false, false);
  _state = _enabled ? WiFiConnectionState::Disconnected : WiFiConnectionState::Disabled;
}

void WiFiController::forgetNetwork() {
  _reconnectAllowed = false;
  _scanInProgress = false;
  WiFi.scanDelete();
  WiFi.disconnect(false, true);
  _ssid = "";
  _networkType = "Wi-Fi";
  _error = "";
  if (_enabled) {
    _state = WiFiConnectionState::Disconnected;
    startScan();
  } else {
    _state = WiFiConnectionState::Disabled;
  }
}

void WiFiController::startScan() {
  if (!_enabled || _scanInProgress || isConnected()) {
    return;
  }

  _error = "";
  _networkCount = 0;
  WiFi.scanDelete();
  int16_t result = WiFi.scanNetworks(true, true);
  if (result == WIFI_SCAN_RUNNING || result == -1) {
    _scanInProgress = true;
    _state = WiFiConnectionState::Scanning;
    return;
  }

  finishScan(result);
}

uint8_t WiFiController::networkCount() const {
  return _networkCount;
}

const WiFiNetworkInfo &WiFiController::networkAt(uint8_t index) const {
  static WiFiNetworkInfo empty;
  return index < _networkCount ? _networks[index] : empty;
}

const char *WiFiController::securityType(uint8_t authMode) {
  switch (authMode) {
    case WIFI_AUTH_OPEN: return "Open";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK: return "WPA3";
    default: return "Secured";
  }
}

void WiFiController::finishScan(int16_t result) {
  _scanInProgress = false;
  if (result < 0) {
    _state = WiFiConnectionState::Disconnected;
    _error = "Scan failed";
    return;
  }

  for (int16_t i = 0; i < result; i++) {
    String name = WiFi.SSID(i);
    if (name.length() == 0) {
      continue;
    }
    addScanNetwork(name, WiFi.RSSI(i), WiFi.encryptionType(i));
  }
  sortNetworks();
  _state = WiFiConnectionState::Disconnected;
}

void WiFiController::addScanNetwork(const String &ssid, int32_t rssi, uint8_t authMode) {
  for (uint8_t i = 0; i < _networkCount; i++) {
    if (_networks[i].ssid == ssid) {
      if (rssi > _networks[i].rssi) {
        _networks[i].rssi = rssi;
        _networks[i].authMode = authMode;
      }
      return;
    }
  }

  if (_networkCount < MaxNetworks) {
    _networks[_networkCount].ssid = ssid;
    _networks[_networkCount].rssi = rssi;
    _networks[_networkCount].authMode = authMode;
    _networkCount++;
    return;
  }

  uint8_t weakest = 0;
  for (uint8_t i = 1; i < _networkCount; i++) {
    if (_networks[i].rssi < _networks[weakest].rssi) {
      weakest = i;
    }
  }
  if (rssi > _networks[weakest].rssi) {
    _networks[weakest].ssid = ssid;
    _networks[weakest].rssi = rssi;
    _networks[weakest].authMode = authMode;
  }
}

void WiFiController::sortNetworks() {
  for (uint8_t i = 1; i < _networkCount; i++) {
    WiFiNetworkInfo value = _networks[i];
    int8_t j = i - 1;
    while (j >= 0 && _networks[j].rssi < value.rssi) {
      _networks[j + 1] = _networks[j];
      j--;
    }
    _networks[j + 1] = value;
  }
}

String WiFiController::savedSsid() const {
  wifi_config_t config = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &config) != ESP_OK ||
      config.sta.ssid[0] == '\0') {
    return "";
  }

  String ssid;
  for (size_t i = 0; i < sizeof(config.sta.ssid) && config.sta.ssid[i] != '\0'; i++) {
    ssid += static_cast<char>(config.sta.ssid[i]);
  }
  return ssid;
}

bool WiFiController::reconnectToSavedNetwork(uint32_t nowMs) {
  String configuredSsid = savedSsid();
  if (configuredSsid.length() == 0) {
    return false;
  }

  _ssid = configuredSsid;
  _state = WiFiConnectionState::Connecting;
  _connectionStartedMs = nowMs;
  WiFi.begin();
  return true;
}

void WiFiController::setConnectedState() {
  _state = WiFiConnectionState::Connected;
  _error = "";
  if (_ssid.length() == 0) {
    _ssid = WiFi.SSID();
  }
  wifi_ap_record_t record;
  if (esp_wifi_sta_get_ap_info(&record) == ESP_OK) {
    _networkType = securityType(record.authmode);
    return;
  }
  for (uint8_t i = 0; i < _networkCount; i++) {
    if (_networks[i].ssid == _ssid) {
      _networkType = securityType(_networks[i].authMode);
      return;
    }
  }
  _networkType = "Wi-Fi";
}

void WiFiController::setFailedState(const String &message) {
  _state = WiFiConnectionState::Failed;
  _error = message;
}
