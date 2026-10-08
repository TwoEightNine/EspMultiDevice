#include "AppPackage.h"

#include <FS.h>
#include <SD.h>
#include <cJSON.h>
#include <esp_heap_caps.h>

#include "../../core/SpiBusLock.h"
#include "../../services/storage/SdCardController.h"

namespace {
String packagePath(const String &id, const char *name) {
  return String("/Apps/") + id + "/" + name;
}

bool readFile(SdCardController &card, const String &path, size_t limit,
              char *&contents, size_t &length, String &error) {
  contents = nullptr;
  length = 0;
  if (!card.isMounted()) { error = "SD card unavailable"; return false; }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(50))) { error = "SD busy"; return false; }
  File file = SD.open(path.c_str(), FILE_READ);
  const bool opened = file && !file.isDirectory();
  const size_t size = opened ? static_cast<size_t>(file.size()) : 0;
  SpiBusLock::unlock();
  if (!opened || size == 0 || size > limit) {
    if (file && SpiBusLock::lock(portMAX_DELAY)) { file.close(); SpiBusLock::unlock(); }
    error = !opened ? "File missing" : "File size invalid";
    return false;
  }
  contents = static_cast<char *>(heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!contents) {
    if (SpiBusLock::lock(portMAX_DELAY)) { file.close(); SpiBusLock::unlock(); }
    error = "App memory unavailable";
    return false;
  }
  while (length < size) {
    if (!card.isMounted() || !SpiBusLock::lock(pdMS_TO_TICKS(50))) {
      error = "SD card unavailable";
      break;
    }
    const size_t chunk = min(static_cast<size_t>(1024), size - length);
    const size_t count = file.readBytes(contents + length, chunk);
    SpiBusLock::unlock();
    if (count != chunk) { error = "Incomplete app file"; break; }
    length += count;
    taskYIELD();
  }
  if (SpiBusLock::lock(portMAX_DELAY)) { file.close(); SpiBusLock::unlock(); }
  if (length != size) {
    heap_caps_free(contents);
    contents = nullptr;
    length = 0;
    return false;
  }
  contents[length] = '\0';
  return true;
}
}

bool AppPackage::validId(const String &value) {
  if (value.isEmpty() || value.length() > 32 || value[0] < 'a' || value[0] > 'z') return false;
  for (size_t i = 1; i < value.length(); ++i) {
    const char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

bool AppPackage::validDataName(const char *value) {
  if (!value) return false;
  const size_t length = strnlen(value, 65);
  if (!length || length > 64 || (length == 1 && value[0] == '.') ||
      (length == 2 && value[0] == '.' && value[1] == '.')) return false;
  for (size_t i = 0; i < length; ++i) {
    const char c = value[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
  }
  return true;
}

AppPackage AppPackage::load(SdCardController &card, const String &id) {
  AppPackage result;
  result.id = id;
  if (!validId(id)) { result.error = "Invalid app ID"; return result; }
  char *json = nullptr;
  size_t length = 0;
  if (!readFile(card, packagePath(id, "manifest.json"), MaxManifestBytes,
                json, length, result.error)) return result;
  cJSON *root = cJSON_ParseWithLength(json, length);
  heap_caps_free(json);
  if (!root || !cJSON_IsObject(root)) {
    result.error = "Invalid manifest JSON";
  } else {
    const cJSON *manifestId = cJSON_GetObjectItemCaseSensitive(root, "id");
    const cJSON *manifestName = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *sdk = cJSON_GetObjectItemCaseSensitive(root, "sdk");
    if (!cJSON_IsString(manifestId) || id != manifestId->valuestring) {
      result.error = "Manifest ID differs from folder";
    } else if (!cJSON_IsString(manifestName) || !manifestName->valuestring ||
               !manifestName->valuestring[0] || strlen(manifestName->valuestring) > 40) {
      result.error = "Invalid app name";
    } else if (!cJSON_IsNumber(sdk) ||
               (sdk->valuedouble != 1 && sdk->valuedouble != 2)) {
      result.error = "Unsupported SDK version";
    } else {
      result.sdk = static_cast<uint8_t>(sdk->valuedouble);
      result.name = manifestName->valuestring;
      if (!SpiBusLock::lock(pdMS_TO_TICKS(50))) {
        result.error = "SD busy";
      } else {
        File source = SD.open(packagePath(id, "main.lua").c_str(), FILE_READ);
        result.valid = source && !source.isDirectory() && source.size() > 0 &&
                       source.size() <= MaxSourceBytes;
        if (source) source.close();
        SpiBusLock::unlock();
        if (!result.valid) result.error = "main.lua missing or too large";
      }
    }
  }
  if (root) cJSON_Delete(root);
  return result;
}

bool AppPackage::readSource(SdCardController &card, const String &id,
                            char *&source, size_t &length, String &error) {
  if (!validId(id)) { error = "Invalid app ID"; return false; }
  return readFile(card, packagePath(id, "main.lua"), MaxSourceBytes,
                  source, length, error);
}
