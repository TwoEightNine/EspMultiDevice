#include "UsbDriveBoot.h"

#include <Preferences.h>

namespace {
const char *PreferencesNamespace = "usb-drive";
const char *NextBootKey = "next";
}

bool UsbDriveBoot::request() {
  Preferences preferences;
  if (!preferences.begin(PreferencesNamespace, false)) return false;
  const bool saved = preferences.putBool(NextBootKey, true) == sizeof(bool);
  preferences.end();
  return saved;
}

bool UsbDriveBoot::consumeRequest() {
  Preferences preferences;
  if (!preferences.begin(PreferencesNamespace, false)) return false;
  const bool requested = preferences.getBool(NextBootKey, false);
  bool cleared = true;
  if (requested) {
    cleared = preferences.remove(NextBootKey) ||
              preferences.putBool(NextBootKey, false) == sizeof(bool);
  }
  preferences.end();
  return requested && cleared;
}
