#include "AppSettings.h"

#include <Preferences.h>

namespace {
constexpr char PreferencesNamespace[] = "apps";
constexpr char DisabledKey[] = "disabled";
}  // namespace

AppSettings::AppSettings() : _mutex(xSemaphoreCreateMutex()) {}

void AppSettings::begin() {
  // Static: the encoded list (about 2 KiB) stays off the loop task's stack.
  static char blob[AppDisabledList::MaxEncodedBytes];
  size_t length = 0;
  Preferences preferences;
  if (preferences.begin(PreferencesNamespace, true)) {
    if (preferences.isKey(DisabledKey)) {
      const size_t stored = preferences.getBytesLength(DisabledKey);
      if (stored <= sizeof(blob)) length = preferences.getBytes(DisabledKey, blob, sizeof(blob));
    }
    preferences.end();
  }
  AppDisabledList loaded;
  if (!loaded.decode(blob, length)) {
    Serial.println(F("[Apps] disabled-app list invalid; all apps enabled"));
  }
  if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
    _disabled = loaded;
    xSemaphoreGive(_mutex);
  }
}

bool AppSettings::isEnabled(const char *id) const {
  if (!_mutex || xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return true;
  const bool disabled = _disabled.contains(id);
  xSemaphoreGive(_mutex);
  return !disabled;
}

bool AppSettings::setEnabled(const char *id, bool enabled) {
  if (!AppDisabledList::validId(id) || !_mutex) return false;
  if (xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return false;
  AppDisabledList next = _disabled;
  xSemaphoreGive(_mutex);
  if (!next.setDisabled(id, !enabled)) return true;
  // Persist before publishing, so the UI never shows a state a reboot loses.
  if (!save(next)) return false;
  if (xSemaphoreTake(_mutex, portMAX_DELAY) != pdTRUE) return false;
  _disabled = next;
  xSemaphoreGive(_mutex);
  return true;
}

bool AppSettings::save(const AppDisabledList &list) {
  static char blob[AppDisabledList::MaxEncodedBytes];
  const size_t length = list.encode(blob, sizeof(blob));
  Preferences preferences;
  if (!preferences.begin(PreferencesNamespace, false)) return false;
  const bool saved = length ? preferences.putBytes(DisabledKey, blob, length) == length
                            : (!preferences.isKey(DisabledKey) || preferences.remove(DisabledKey));
  preferences.end();
  return saved;
}
