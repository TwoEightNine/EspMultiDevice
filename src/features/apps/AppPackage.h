#pragma once

#include <Arduino.h>

class SdCardController;

struct AppPackage {
  static constexpr size_t MaxManifestBytes = 2048;
  static constexpr size_t MaxSourceBytes = 65536;
  static constexpr size_t MaxDataBytes = 16384;
  static constexpr uint16_t DefaultIconBackground = 0x6318;
  // How long one app SD transaction waits for the shared SD/touch bus. Battery
  // history appends hold it for about 50 ms every second, so a shorter wait
  // fails app starts and storage calls at random. Callers exclude the wait
  // from the callback time limit.
  static constexpr TickType_t SdLockTimeout = pdMS_TO_TICKS(250);
  static constexpr uint8_t MaxSdk = 7;

  String id;
  String name;
  String error;
  uint8_t sdk = 0;
  uint16_t iconBackground = DefaultIconBackground;
  bool hasIconBackground = false;
  // Manifest `show_in_home`: Home shows a tile. False leaves the app reachable
  // only through another app's `esp.apps.open` (SDK 7).
  bool showInHome = true;
  // SDK 4 manifest grant for `esp.fs` general SD access.
  bool filesPermission = false;
  // SDK 6 manifest grant for `esp.net`, limited to the listed hosts.
  static constexpr uint8_t MaxHosts = 8;
  bool networkPermission = false;
  String hosts[MaxHosts];
  uint8_t hostCount = 0;
  bool valid = false;
  // False when the manifest could not be read at all (folder or manifest
  // missing, or the card unavailable); `error` still explains why.
  bool installed = false;

  static bool validId(const String &value);
  static bool validDataName(const char *value);
  static bool validHost(const char *value);
  static AppPackage load(SdCardController &card, const String &id);
  static bool readSource(SdCardController &card, const String &id,
                         char *&source, size_t &length, String &error);
};
