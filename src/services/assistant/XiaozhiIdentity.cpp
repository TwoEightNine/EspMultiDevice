#include "XiaozhiIdentity.h"

#include <Preferences.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <string.h>

namespace {
constexpr char PreferencesNamespace[] = "assistant";
constexpr char ClientUuidKey[] = "client_uuid";
}

XiaozhiIdentity::XiaozhiIdentity() : _deviceId{}, _clientId{}, _lastError(F("Not initialized")) {}

bool XiaozhiIdentity::begin() {
  uint8_t mac[6] = {};
  if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
    _lastError = F("Cannot read Wi-Fi station MAC");
    return false;
  }
  snprintf(_deviceId, sizeof(_deviceId), "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  Preferences preferences;
  if (!preferences.begin(PreferencesNamespace, false)) {
    _lastError = F("Cannot open assistant NVS namespace");
    return false;
  }

  String stored = preferences.getString(ClientUuidKey, "");
  if (stored.length() == 0) {
    generateUuid(_clientId);
    const size_t written = preferences.putString(ClientUuidKey, _clientId);
    if (written != strlen(_clientId)) {
      preferences.end();
      memset(_clientId, 0, sizeof(_clientId));
      _lastError = F("Cannot persist assistant client UUID");
      return false;
    }
    stored = preferences.getString(ClientUuidKey, "");
  }
  preferences.end();

  if (!validUuid(stored.c_str())) {
    memset(_clientId, 0, sizeof(_clientId));
    _lastError = F("Stored assistant client UUID is malformed");
    return false;
  }
  memcpy(_clientId, stored.c_str(), sizeof(_clientId));
  _lastError = F("OK");
  return true;
}

bool XiaozhiIdentity::validUuid(const char *value) {
  if (!value || strlen(value) != ClientIdBytes - 1) return false;
  for (size_t i = 0; i < ClientIdBytes - 1; ++i) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
      if (value[i] != '-') return false;
      continue;
    }
    const bool digit = value[i] >= '0' && value[i] <= '9';
    const bool lowerHex = value[i] >= 'a' && value[i] <= 'f';
    if (!digit && !lowerHex) return false;
  }
  return value[14] == '4' && (value[19] == '8' || value[19] == '9' ||
                             value[19] == 'a' || value[19] == 'b');
}

void XiaozhiIdentity::generateUuid(char *output) {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  bytes[6] = static_cast<uint8_t>((bytes[6] & 0x0fU) | 0x40U);
  bytes[8] = static_cast<uint8_t>((bytes[8] & 0x3fU) | 0x80U);
  snprintf(output, ClientIdBytes,
           "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
           bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
}
