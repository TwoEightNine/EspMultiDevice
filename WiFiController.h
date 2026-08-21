#pragma once

#include <Arduino.h>
#include <WiFi.h>

enum class WiFiConnectionState : uint8_t {
  Disabled,
  Disconnected,
  Scanning,
  Connecting,
  Connected,
  Failed
};

struct WiFiNetworkInfo {
  String ssid;
  int32_t rssi;
  uint8_t authMode;

  WiFiNetworkInfo() : ssid(), rssi(0), authMode(WIFI_AUTH_OPEN) {
  }
};

class WiFiController {
public:
  static const uint8_t MaxNetworks = 12;
  static const uint32_t ConnectionTimeoutMs = 15000;

  WiFiController();

  void begin();
  void update(uint32_t nowMs);

  void setEnabled(bool enabled);
  bool isEnabled() const;
  bool isConnected() const;
  bool isScanning() const;
  WiFiConnectionState state() const;

  const String &ssid() const;
  const String &networkType() const;
  const String &error() const;

  bool connect(const String &ssid, const String &password);
  void disconnect();
  void forgetNetwork();

  void startScan();
  uint8_t networkCount() const;
  const WiFiNetworkInfo &networkAt(uint8_t index) const;

  static const char *securityType(uint8_t authMode);

private:
  bool _enabled;
  bool _scanInProgress;
  bool _reconnectAllowed;
  uint32_t _connectionStartedMs;
  WiFiConnectionState _state;
  String _ssid;
  String _networkType;
  String _error;
  WiFiNetworkInfo _networks[MaxNetworks];
  uint8_t _networkCount;

  void finishScan(int16_t result);
  void addScanNetwork(const String &ssid, int32_t rssi, uint8_t authMode);
  void sortNetworks();
  void setConnectedState();
  void setFailedState(const String &message);
};
