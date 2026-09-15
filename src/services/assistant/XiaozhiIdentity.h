#pragma once

#include <Arduino.h>

class XiaozhiIdentity {
public:
  static constexpr size_t DeviceIdBytes = 18;
  static constexpr size_t ClientIdBytes = 37;

  XiaozhiIdentity();

  bool begin();
  const char *deviceId() const { return _deviceId; }
  const char *clientId() const { return _clientId; }
  const String &lastError() const { return _lastError; }

private:
  char _deviceId[DeviceIdBytes];
  char _clientId[ClientIdBytes];
  String _lastError;

  static bool validUuid(const char *value);
  static void generateUuid(char *output);
};
