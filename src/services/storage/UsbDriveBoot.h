#pragma once

class UsbDriveBoot {
public:
  // Persists a one-shot request consumed by the next firmware boot.
  static bool request();
  static bool consumeRequest();
};
