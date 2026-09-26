#include "LedSettings.h"

#include <Preferences.h>
#include <esp32-hal-rmt.h>

namespace {
constexpr char PreferencesNamespace[] = "led";
constexpr char EnabledKey[] = "enabled";
constexpr char ColorKey[] = "color";
constexpr uint32_t RmtFrequencyHz = 10000000;
constexpr uint32_t WriteTimeoutMs = 5;
constexpr uint8_t Ws2812ZeroHighTicks = 4;
constexpr uint8_t Ws2812ZeroLowTicks = 8;
constexpr uint8_t Ws2812OneHighTicks = 8;
constexpr uint8_t Ws2812OneLowTicks = 4;

struct Rgb {
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};

Rgb rgbFor(LedColor color) {
  switch (color) {
    case LedColor::Red: return {255, 0, 0};
    case LedColor::Green: return {0, 255, 0};
    case LedColor::Blue: return {0, 0, 255};
    case LedColor::Yellow: return {255, 255, 0};
    case LedColor::Cyan: return {0, 255, 255};
    case LedColor::Magenta: return {255, 0, 255};
  }
  return {0, 255, 255};
}
}  // namespace

LedSettings::LedSettings(int pin)
  : _pin(pin),
    _available(false),
    _enabled(DefaultEnabled),
    _color(DefaultColor),
    _error() {
}

bool LedSettings::begin() {
  Preferences preferences;
  if (preferences.begin(PreferencesNamespace, false)) {
    _enabled = preferences.getBool(EnabledKey, DefaultEnabled);
    const uint8_t storedColor = preferences.getUChar(ColorKey,
                                                     static_cast<uint8_t>(DefaultColor));
    if (isValidColor(storedColor)) {
      _color = static_cast<LedColor>(storedColor);
    } else {
      _color = DefaultColor;
      preferences.putUChar(ColorKey, static_cast<uint8_t>(_color));
    }
    preferences.end();
  }

  if (!rmtInit(_pin, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, RmtFrequencyHz) ||
      !rmtSetEOT(_pin, LOW)) {
    _error = F("LED hardware unavailable");
    return false;
  }

  _available = true;
  return apply();
}

bool LedSettings::isAvailable() const {
  return _available;
}

bool LedSettings::isEnabled() const {
  return _enabled;
}

LedColor LedSettings::color() const {
  return _color;
}

const String &LedSettings::error() const {
  return _error;
}

bool LedSettings::setEnabled(bool enabled) {
  if (!_available) {
    _error = F("LED hardware unavailable");
    return false;
  }

  if (_enabled == enabled) return true;
  const bool previous = _enabled;
  _enabled = enabled;
  if (apply() && save()) return true;
  _enabled = previous;
  (void)apply();
  return false;
}

bool LedSettings::setColor(LedColor color) {
  if (!_available) {
    _error = F("LED hardware unavailable");
    return false;
  }

  if (_color == color) return true;
  const LedColor previous = _color;
  _color = color;
  if (apply() && save()) return true;
  _color = previous;
  (void)apply();
  return false;
}

const char *LedSettings::colorName(LedColor color) {
  switch (color) {
    case LedColor::Red: return "Red";
    case LedColor::Green: return "Green";
    case LedColor::Blue: return "Blue";
    case LedColor::Yellow: return "Yellow";
    case LedColor::Cyan: return "Cyan";
    case LedColor::Magenta: return "Magenta";
  }
  return "Cyan";
}

uint16_t LedSettings::color565(LedColor color) {
  const Rgb rgb = rgbFor(color);
  return static_cast<uint16_t>(((rgb.red & 0xf8) << 8) |
                               ((rgb.green & 0xfc) << 3) |
                               (rgb.blue >> 3));
}

bool LedSettings::apply() {
  rmt_data_t symbols[24];
  const Rgb rgb = _enabled ? rgbFor(_color) : Rgb{0, 0, 0};
  const uint8_t bytes[] = {rgb.green, rgb.red, rgb.blue};
  uint8_t symbol = 0;

  for (uint8_t byte : bytes) {
    for (int8_t bit = 7; bit >= 0; bit--) {
      const bool one = (byte & (1 << bit)) != 0;
      symbols[symbol].level0 = HIGH;
      symbols[symbol].duration0 = one ? Ws2812OneHighTicks : Ws2812ZeroHighTicks;
      symbols[symbol].level1 = LOW;
      symbols[symbol].duration1 = one ? Ws2812OneLowTicks : Ws2812ZeroLowTicks;
      symbol++;
    }
  }

  if (!rmtWrite(_pin, symbols, RMT_SYMBOLS_OF(symbols), WriteTimeoutMs)) {
    _error = F("LED update failed");
    return false;
  }

  _error = "";
  return true;
}

bool LedSettings::save() {
  Preferences preferences;
  if (!preferences.begin(PreferencesNamespace, false)) {
    _error = F("LED setting not saved");
    return false;
  }

  const bool saved = preferences.putBool(EnabledKey, _enabled) == sizeof(uint8_t) &&
                     preferences.putUChar(ColorKey, static_cast<uint8_t>(_color)) ==
                         sizeof(uint8_t);
  preferences.end();
  if (!saved) {
    _error = F("LED setting not saved");
    return false;
  }

  _error = "";
  return true;
}

bool LedSettings::isValidColor(uint8_t value) {
  return value < ColorCount;
}
