#pragma once

#include <Arduino.h>

// Firmware release version (semantic versioning). Edit only the three numeric
// components; the display string is derived from them so it cannot drift.
#define ESP_MEDIA_VERSION_MAJOR 0
#define ESP_MEDIA_VERSION_MINOR 1
#define ESP_MEDIA_VERSION_PATCH 0

#define ESP_MEDIA_VERSION_STRINGIFY_(value) #value
#define ESP_MEDIA_VERSION_STRINGIFY(value) ESP_MEDIA_VERSION_STRINGIFY_(value)

namespace FirmwareVersion {

constexpr uint8_t Major = ESP_MEDIA_VERSION_MAJOR;
constexpr uint8_t Minor = ESP_MEDIA_VERSION_MINOR;
constexpr uint8_t Patch = ESP_MEDIA_VERSION_PATCH;
constexpr const char *Text =
  ESP_MEDIA_VERSION_STRINGIFY(ESP_MEDIA_VERSION_MAJOR) "."
  ESP_MEDIA_VERSION_STRINGIFY(ESP_MEDIA_VERSION_MINOR) "."
  ESP_MEDIA_VERSION_STRINGIFY(ESP_MEDIA_VERSION_PATCH);

}  // namespace FirmwareVersion
