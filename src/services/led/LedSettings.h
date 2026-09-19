#pragma once

#include <Arduino.h>

enum class LedColor : uint8_t {
  Red = 0,
  Green,
  Blue,
  Yellow,
  Cyan,
  Magenta
};

// Owns the initial ESP32-S3-DevKitC-1's single WS2812-compatible RGB LED and
// its persisted user preference. The GPIO is supplied by the composition root
// because it is a board wiring decision.
class LedSettings {
public:
  static const uint8_t ColorCount = 6;
  static const bool DefaultEnabled = false;
  static const LedColor DefaultColor = LedColor::Cyan;

  explicit LedSettings(int pin);

  bool begin();
  bool isAvailable() const;
  bool isEnabled() const;
  LedColor color() const;
  const String &error() const;

  bool setEnabled(bool enabled);
  bool setColor(LedColor color);

  static const char *colorName(LedColor color);
  static uint16_t color565(LedColor color);

private:
  int _pin;
  bool _available;
  bool _enabled;
  LedColor _color;
  String _error;

  bool apply();
  bool save();
  static bool isValidColor(uint8_t value);
};
