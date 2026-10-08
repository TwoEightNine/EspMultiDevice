#pragma once

#include <Arduino.h>

class SdCardController;

struct AppPackage {
  static constexpr size_t MaxManifestBytes = 2048;
  static constexpr size_t MaxSourceBytes = 65536;
  static constexpr size_t MaxDataBytes = 16384;

  String id;
  String name;
  String error;
  uint8_t sdk = 0;
  bool valid = false;

  static bool validId(const String &value);
  static bool validDataName(const char *value);
  static AppPackage load(SdCardController &card, const String &id);
  static bool readSource(SdCardController &card, const String &id,
                         char *&source, size_t &length, String &error);
};
