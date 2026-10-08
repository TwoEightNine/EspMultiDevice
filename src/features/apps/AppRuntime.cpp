#include "AppRuntime.h"

#include <FS.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <math.h>

#include "../../vendor_shims/lua/LuaConfig.h"

extern "C" {
#include "../../../third_party/lua/lua.h"
#include "../../../third_party/lua/lauxlib.h"
#include "../../../third_party/lua/lualib.h"
}

#include "../../core/SpiBusLock.h"
#include "../../services/storage/SdCardController.h"
#include "../../services/audio/SpeakerController.h"
#include "../../services/audio/AudioSettings.h"
#include "AppPackage.h"

namespace {
String dataDirectory(const String &id) { return String("/.system/appdata/") + id; }
String dataPath(const String &id, const char *name) { return dataDirectory(id) + "/" + name; }

void copyText(char *target, size_t capacity, const char *source) {
  if (!source) source = "";
  snprintf(target, capacity, "%s", source);
}

void removeGlobal(lua_State *state, const char *name) {
  lua_pushnil(state);
  lua_setglobal(state, name);
}
}

AppRuntime::AppRuntime(SdCardController &card, SpeakerController &speaker,
                       AudioSettings &audioSettings)
  : _card(card), _speaker(speaker), _audioSettings(audioSettings) {}

bool AppRuntime::start(const String &id, const Status &initialStatus) {
  if (_task || !AppPackage::validId(id)) return false;
  _id = id;
  _cancel = false;
  _stopped = false;
  _revision = 0;
  _scene = Scene{};
  _frameRevision = 0;
  _frameOpen = false;
  _frameCost = 0;
  _sdkVersion = 1;
  _toneLastWriteMs = 0;
  _buildingFrame = static_cast<Frame *>(heap_caps_calloc(1, sizeof(Frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  _publishedFrame = static_cast<Frame *>(heap_caps_calloc(1, sizeof(Frame), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!_buildingFrame || !_publishedFrame) {
    if (_buildingFrame) heap_caps_free(_buildingFrame);
    if (_publishedFrame) heap_caps_free(_publishedFrame);
    _buildingFrame = _publishedFrame = nullptr;
    _stopped = true;
    return false;
  }
  _status = initialStatus;
  _actions = xQueueCreate(8, sizeof(Action));
  _mutex = xSemaphoreCreateMutex();
  if (!_actions || !_mutex ||
      xTaskCreatePinnedToCore(taskMain, "sd-app", WorkerStackBytes, this,
                              tskIDLE_PRIORITY, &_task, 0) != pdPASS) {
    if (_actions) { vQueueDelete(_actions); _actions = nullptr; }
    if (_mutex) { vSemaphoreDelete(_mutex); _mutex = nullptr; }
    heap_caps_free(_buildingFrame); heap_caps_free(_publishedFrame);
    _buildingFrame = _publishedFrame = nullptr;
    _stopped = true;
    return false;
  }
  return true;
}

void AppRuntime::requestStop() { _cancel = true; }
bool AppRuntime::stopped() const { return _stopped; }

bool AppRuntime::end() {
  requestStop();
  if (!_stopped) return false;
  if (_task) { vTaskDelete(_task); _task = nullptr; }
  if (_actions) { vQueueDelete(_actions); _actions = nullptr; }
  if (_mutex) { vSemaphoreDelete(_mutex); _mutex = nullptr; }
  clearTones();
  if (_buildingFrame) { heap_caps_free(_buildingFrame); _buildingFrame = nullptr; }
  if (_publishedFrame) { heap_caps_free(_publishedFrame); _publishedFrame = nullptr; }
  _id = String();
  _displayName = String();
  return true;
}

bool AppRuntime::sendAction(const char *id) {
  if (!_actions || _cancel || _stopped || !id) return false;
  Action action{};
  copyText(action.id, sizeof(action.id), id);
  return xQueueSend(_actions, &action, 0) == pdTRUE;
}

bool AppRuntime::sendTouch(uint16_t x, uint16_t y) {
  if (!_actions || _cancel || _stopped) return false;
  Action action{};
  action.x = x; action.y = y; action.touch = true;
  return xQueueSend(_actions, &action, 0) == pdTRUE;
}

bool AppRuntime::takeScene(Scene &scene, uint32_t &revision) {
  if (!_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
  const bool changed = _revision != revision;
  if (changed) { scene = _scene; revision = _revision; }
  xSemaphoreGive(_mutex);
  return changed;
}

bool AppRuntime::takeFrame(Frame &frame, uint32_t &revision) {
  if (!_mutex || !_publishedFrame || xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
  const bool changed = _frameRevision != revision;
  if (changed) { frame = *_publishedFrame; revision = _frameRevision; }
  xSemaphoreGive(_mutex);
  return changed;
}

void AppRuntime::publishStatus(const Status &status) {
  if (!_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return;
  _status = status;
  xSemaphoreGive(_mutex);
}

void *AppRuntime::luaAlloc(void *context, void *pointer, size_t oldSize, size_t newSize) {
  AppRuntime *runtime = static_cast<AppRuntime *>(context);
  const size_t countedOld = pointer ? oldSize : 0;
  if (newSize == 0) {
    if (pointer) {
      heap_caps_free(pointer);
      runtime->_luaBytes -= countedOld;
    }
    return nullptr;
  }
  if (countedOld > runtime->_luaBytes ||
      newSize > LuaMemoryLimit - (runtime->_luaBytes - countedOld)) return nullptr;
  void *next = heap_caps_realloc(pointer, newSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (next) runtime->_luaBytes = runtime->_luaBytes - countedOld + newSize;
  return next;
}

void AppRuntime::luaHook(lua_State *state, lua_Debug *) {
  AppRuntime *runtime = *static_cast<AppRuntime **>(lua_getextraspace(state));
  runtime->_hookCount += 1000;
  if (runtime->_cancel || runtime->_hookCount > CallbackMaxInstructions ||
      esp_timer_get_time() - runtime->_callbackStartedUs > CallbackMaxUs) {
    luaL_error(state, "app callback exceeded its time or instruction limit");
  }
}

AppRuntime *AppRuntime::self(lua_State *state) {
  return static_cast<AppRuntime *>(lua_touserdata(state, lua_upvalueindex(1)));
}

int AppRuntime::luaShow(lua_State *state) {
  AppRuntime *runtime = self(state);
  luaL_checktype(state, 1, LUA_TTABLE);
  const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, 1));
  if (count > MaxRows) return luaL_error(state, "too many UI rows");
  Scene scene{};
  lua_getfield(state, 1, "title");
  size_t titleLength = 0;
  const char *title = lua_tolstring(state, -1, &titleLength);
  if (title && (titleLength > 40 || memchr(title, 0, titleLength)))
    return luaL_error(state, "UI title invalid");
  copyText(scene.title, sizeof(scene.title), title ? title : runtime->_displayName.c_str());
  lua_pop(state, 1);
  scene.count = static_cast<uint8_t>(count);
  for (lua_Integer index = 1; index <= count; ++index) {
    lua_rawgeti(state, 1, index);
    luaL_checktype(state, -1, LUA_TTABLE);
    Row &row = scene.rows[index - 1];
    lua_getfield(state, -1, "kind");
    const char *kind = luaL_checkstring(state, -1);
    if (strcmp(kind, "text") != 0 && strcmp(kind, "button") != 0)
      return luaL_error(state, "unsupported UI row kind");
    row.button = strcmp(kind, "button") == 0;
    lua_pop(state, 1);
    lua_getfield(state, -1, "text");
    size_t textLength = 0;
    const char *text = luaL_checklstring(state, -1, &textLength);
    if (!textLength || textLength > 96 || memchr(text, 0, textLength))
      return luaL_error(state, "UI text length invalid");
    copyText(row.text, sizeof(row.text), text);
    lua_pop(state, 1);
    if (row.button) {
      lua_getfield(state, -1, "id");
      size_t idLength = 0;
      const char *id = luaL_checklstring(state, -1, &idLength);
      if (!idLength || idLength > 32 || memchr(id, 0, idLength))
        return luaL_error(state, "UI action ID invalid");
      copyText(row.id, sizeof(row.id), id);
      lua_pop(state, 1);
    }
    lua_pop(state, 1);
  }
  if (xSemaphoreTake(runtime->_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    runtime->_scene = scene;
    ++runtime->_revision;
    xSemaphoreGive(runtime->_mutex);
  }
  return 0;
}

bool AppRuntime::readData(const char *name, char *&data, size_t &length, String &error) {
  data = nullptr;
  length = 0;
  if (!AppPackage::validDataName(name)) { error = "Invalid file name"; return false; }
  if (!_card.isMounted()) { error = "SD card unavailable"; return false; }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(50))) { error = "SD busy"; return false; }
  File file = SD.open(dataPath(_id, name).c_str(), FILE_READ);
  const bool opened = file && !file.isDirectory();
  const size_t size = opened ? static_cast<size_t>(file.size()) : 0;
  SpiBusLock::unlock();
  if (!opened || size > AppPackage::MaxDataBytes) {
    if (file && SpiBusLock::lock(portMAX_DELAY)) { file.close(); SpiBusLock::unlock(); }
    error = !opened ? "File not found" : "File too large";
    return false;
  }
  data = static_cast<char *>(heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!data) {
    if (SpiBusLock::lock(portMAX_DELAY)) { file.close(); SpiBusLock::unlock(); }
    error = "App memory unavailable";
    return false;
  }
  while (length < size && !_cancel && _card.isMounted()) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(50))) { error = "SD busy"; break; }
    const size_t chunk = min(static_cast<size_t>(1024), size - length);
    const size_t count = file.readBytes(data + length, chunk);
    SpiBusLock::unlock();
    if (count != chunk) { error = "Incomplete read"; break; }
    length += count;
    taskYIELD();
  }
  if (SpiBusLock::lock(portMAX_DELAY)) { file.close(); SpiBusLock::unlock(); }
  if (length != size || _cancel) {
    if (error.isEmpty()) error = "Read interrupted";
    heap_caps_free(data); data = nullptr; length = 0;
    return false;
  }
  data[length] = '\0';
  return true;
}

bool AppRuntime::writeData(const char *name, const char *data, size_t length, String &error) {
  if (!AppPackage::validDataName(name) || length > AppPackage::MaxDataBytes) {
    error = "Invalid file name or size"; return false;
  }
  if (!_card.isMounted()) { error = "SD card unavailable"; return false; }
  const String directory = dataDirectory(_id);
  const String path = dataPath(_id, name);
  const String temp = path + ".tmp";
  if (!SpiBusLock::lock(pdMS_TO_TICKS(50))) { error = "SD busy"; return false; }
  bool ready = (SD.exists("/.system") || SD.mkdir("/.system")) &&
               (SD.exists("/.system/appdata") || SD.mkdir("/.system/appdata")) &&
               (SD.exists(directory.c_str()) || SD.mkdir(directory.c_str()));
  if (ready) SD.remove(temp.c_str());
  File file = ready ? SD.open(temp.c_str(), FILE_WRITE) : File();
  SpiBusLock::unlock();
  if (!file) { error = "Cannot open app data"; return false; }
  size_t written = 0;
  while (written < length && !_cancel && _card.isMounted()) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(50))) { error = "SD busy"; break; }
    const size_t chunk = min(static_cast<size_t>(1024), length - written);
    const size_t count = file.write(reinterpret_cast<const uint8_t *>(data + written), chunk);
    SpiBusLock::unlock();
    if (count != chunk) { error = "Incomplete write"; break; }
    written += count;
    taskYIELD();
  }
  bool closed = false;
  if (SpiBusLock::lock(portMAX_DELAY)) {
    file.flush(); file.close();
    SpiBusLock::unlock();
    closed = true;
  }
  bool success = closed && written == length && !_cancel && _card.isMounted();
  if (success && SpiBusLock::lock(pdMS_TO_TICKS(50))) {
    if (SD.exists(path.c_str())) SD.remove(path.c_str());
    success = SD.rename(temp.c_str(), path.c_str());
    SpiBusLock::unlock();
  } else success = false;
  if (!success) {
    if (SpiBusLock::lock(pdMS_TO_TICKS(50))) { SD.remove(temp.c_str()); SpiBusLock::unlock(); }
    if (error.isEmpty()) error = "App data write failed";
  }
  return success;
}

int AppRuntime::luaRead(lua_State *state) {
  AppRuntime *runtime = self(state);
  size_t nameLength = 0;
  const char *name = luaL_checklstring(state, 1, &nameLength);
  if (nameLength > 64 || memchr(name, 0, nameLength)) {
    lua_pushnil(state); lua_pushliteral(state, "Invalid file name"); return 2;
  }
  char *data = nullptr;
  size_t length = 0;
  String error;
  const int64_t started = esp_timer_get_time();
  if (!runtime->readData(name, data, length, error)) {
    runtime->_callbackStartedUs += esp_timer_get_time() - started;
    lua_pushnil(state); lua_pushstring(state, error.c_str()); return 2;
  }
  runtime->_callbackStartedUs += esp_timer_get_time() - started;
  lua_pushlstring(state, data, length);
  heap_caps_free(data);
  return 1;
}

int AppRuntime::luaWrite(lua_State *state) {
  AppRuntime *runtime = self(state);
  size_t nameLength = 0;
  const char *name = luaL_checklstring(state, 1, &nameLength);
  if (nameLength > 64 || memchr(name, 0, nameLength)) {
    lua_pushboolean(state, false); lua_pushliteral(state, "Invalid file name"); return 2;
  }
  size_t length = 0;
  const char *data = luaL_checklstring(state, 2, &length);
  String error;
  const int64_t started = esp_timer_get_time();
  const bool success = runtime->writeData(name, data, length, error);
  runtime->_callbackStartedUs += esp_timer_get_time() - started;
  lua_pushboolean(state, success);
  if (!success) { lua_pushstring(state, error.c_str()); return 2; }
  return 1;
}

int AppRuntime::luaStatus(lua_State *state) {
  AppRuntime *runtime = self(state);
  Status status{};
  if (xSemaphoreTake(runtime->_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    status = runtime->_status;
    xSemaphoreGive(runtime->_mutex);
  }
  lua_createtable(state, 0, 4);
  lua_pushinteger(state, status.batteryVoltageMv); lua_setfield(state, -2, "battery_voltage_mv");
  lua_pushboolean(state, status.wifiConnected); lua_setfield(state, -2, "wifi_connected");
  lua_pushboolean(state, status.sdMounted); lua_setfield(state, -2, "sd_mounted");
  lua_pushinteger(state, status.epochSeconds); lua_setfield(state, -2, "epoch_seconds");
  return 1;
}

int AppRuntime::luaMillis(lua_State *state) {
  lua_pushinteger(state, millis());
  return 1;
}

int AppRuntime::luaBeginFrame(lua_State *state) {
  AppRuntime *runtime = self(state);
  if (runtime->_sdkVersion != 2 || !runtime->_buildingFrame)
    return luaL_error(state, "SDK 2 frame unavailable");
  const lua_Integer background = luaL_checkinteger(state, 1);
  if (background < 0 || background > 65535) return luaL_error(state, "invalid background color");
  size_t titleLength = 0;
  const char *title = luaL_optlstring(state, 2, runtime->_displayName.c_str(), &titleLength);
  if (titleLength > 40 || memchr(title, 0, titleLength)) return luaL_error(state, "invalid frame title");
  runtime->_buildingFrame->count = 0;
  runtime->_frameCost = 0;
  runtime->_buildingFrame->background = static_cast<uint16_t>(background);
  copyText(runtime->_buildingFrame->title, sizeof(runtime->_buildingFrame->title), title);
  runtime->_frameOpen = true;
  return 0;
}

int AppRuntime::luaPresent(lua_State *state) {
  AppRuntime *runtime = self(state);
  if (!runtime->_frameOpen) return luaL_error(state, "begin_frame required");
  if (xSemaphoreTake(runtime->_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return luaL_error(state, "frame publisher busy");
  Frame *old = runtime->_publishedFrame;
  runtime->_publishedFrame = runtime->_buildingFrame;
  runtime->_buildingFrame = old;
  ++runtime->_frameRevision;
  xSemaphoreGive(runtime->_mutex);
  runtime->_frameOpen = false;
  return 0;
}

int AppRuntime::luaDraw(lua_State *state) {
  AppRuntime *runtime = self(state);
  if (!runtime->_frameOpen || !runtime->_buildingFrame)
    return luaL_error(state, "begin_frame required");
  Frame &frame = *runtime->_buildingFrame;
  if (frame.count >= MaxDrawCommands) return luaL_error(state, "too many draw commands");
  const DrawOp op = static_cast<DrawOp>(lua_tointeger(state, lua_upvalueindex(2)));
  DrawCommand &command = frame.commands[frame.count];
  command = DrawCommand{};
  command.op = op;
  int arity = 0;
  switch (op) {
    case DrawOp::FillScreen: arity = 1; break;
    case DrawOp::FillRect: case DrawOp::DrawRect: case DrawOp::DrawLine:
    case DrawOp::DrawSmoothCircle: case DrawOp::FillSmoothCircle: arity = 5; break;
    case DrawOp::DrawRoundRect: case DrawOp::FillRoundRect: arity = 6; break;
    case DrawOp::HLine: case DrawOp::VLine:
    case DrawOp::DrawCircle: case DrawOp::FillCircle: arity = 4; break;
    case DrawOp::FillTriangle: case DrawOp::DrawWideLine: arity = 7; break;
    case DrawOp::DrawSmoothRoundRect: arity = 8; break;
    case DrawOp::FillSmoothRoundRect: arity = 7; break;
    case DrawOp::DrawSmoothArc: arity = 9; break;
    case DrawOp::Text: arity = 6; break;
    case DrawOp::Button: arity = 7; break;
    case DrawOp::Progress: arity = 8; break;
  }
  if (op == DrawOp::Text || op == DrawOp::Button) {
    const int textIndex = op == DrawOp::Text ? 1 : 5;
    size_t length = 0;
    const char *label = luaL_checklstring(state, textIndex, &length);
    if (!length || length > 96 || memchr(label, 0, length))
      return luaL_error(state, "invalid draw text");
    copyText(command.text, sizeof(command.text), label);
  }
  uint16_t colorMask = 0;
  switch (op) {
    case DrawOp::FillScreen: colorMask = 1U << 0; break;
    case DrawOp::FillRect: case DrawOp::DrawRect: case DrawOp::DrawLine:
    case DrawOp::HLine: case DrawOp::VLine:
    case DrawOp::DrawCircle: case DrawOp::FillCircle:
      colorMask = 1U << (arity - 1); break;
    case DrawOp::DrawRoundRect: case DrawOp::FillRoundRect:
    case DrawOp::FillTriangle: colorMask = 1U << (arity - 1); break;
    case DrawOp::DrawWideLine: case DrawOp::FillSmoothRoundRect:
      colorMask = (1U << 5) | (1U << 6); break;
    case DrawOp::DrawSmoothCircle: case DrawOp::FillSmoothCircle:
      colorMask = (1U << 3) | (1U << 4); break;
    case DrawOp::DrawSmoothRoundRect: case DrawOp::DrawSmoothArc:
      colorMask = (1U << 6) | (1U << 7); break;
    case DrawOp::Text: colorMask = (1U << 2) | (1U << 3); break;
    case DrawOp::Button: colorMask = (1U << 4) | (1U << 5) | (1U << 6); break;
    case DrawOp::Progress: colorMask = (1U << 5) | (1U << 6) | (1U << 7); break;
  }
  for (int i = 0; i < arity; ++i) {
    const int source = op == DrawOp::Text ? i + 2 :
                       (op == DrawOp::Button && i >= 4 ? i + 2 : i + 1);
    if (op == DrawOp::DrawWideLine && i == 4) {
      const lua_Number width = luaL_checknumber(state, source);
      if (!isfinite(width) || width < 0.1 || width > 32.0)
        return luaL_error(state, "invalid line width");
      command.arg[i] = static_cast<int32_t>(width * 10.0 + 0.5);
    } else {
      const lua_Integer value = luaL_checkinteger(state, source);
      if ((colorMask & (1U << i)) ? (value < 0 || value > 65535) :
          (value < -512 || value > 1024))
        return luaL_error(state, "draw argument out of range");
      command.arg[i] = static_cast<int32_t>(value);
    }
  }
  int widthIndex = -1, heightIndex = -1, radiusIndex = -1;
  switch (op) {
    case DrawOp::FillRect: case DrawOp::DrawRect: case DrawOp::Button:
    case DrawOp::Progress: widthIndex = 2; heightIndex = 3; break;
    case DrawOp::HLine: case DrawOp::VLine: widthIndex = 2; break;
    case DrawOp::DrawCircle: case DrawOp::FillCircle:
    case DrawOp::DrawSmoothCircle: case DrawOp::FillSmoothCircle:
    case DrawOp::DrawSmoothArc: radiusIndex = 2; break;
    case DrawOp::DrawRoundRect: case DrawOp::FillRoundRect:
      widthIndex = 2; heightIndex = 3; radiusIndex = 4; break;
    case DrawOp::DrawSmoothRoundRect:
      radiusIndex = 2; widthIndex = 4; heightIndex = 5; break;
    case DrawOp::FillSmoothRoundRect:
      widthIndex = 2; heightIndex = 3; radiusIndex = 4; break;
    default: break;
  }
  if ((widthIndex >= 0 && (command.arg[widthIndex] <= 0 || command.arg[widthIndex] > 640)) ||
      (heightIndex >= 0 && (command.arg[heightIndex] <= 0 || command.arg[heightIndex] > 640)) ||
      (radiusIndex >= 0 && (command.arg[radiusIndex] <= 0 || command.arg[radiusIndex] > 480)))
    return luaL_error(state, "invalid draw geometry");
  if (op == DrawOp::DrawSmoothArc &&
      (command.arg[3] < 0 || command.arg[3] >= command.arg[2] ||
       command.arg[4] < 0 || command.arg[4] > 360 ||
       command.arg[5] < 0 || command.arg[5] > 360 ||
       command.arg[8] < 0 || command.arg[8] > 1))
    return luaL_error(state, "invalid arc geometry");
  if (op == DrawOp::DrawSmoothRoundRect &&
      (command.arg[3] < 0 || command.arg[3] >= command.arg[2] ||
       command.arg[2] > min(command.arg[4], command.arg[5]) / 2))
    return luaL_error(state, "invalid rounded rectangle geometry");
  if ((op == DrawOp::DrawRoundRect || op == DrawOp::FillRoundRect ||
       op == DrawOp::FillSmoothRoundRect) &&
      command.arg[4] > min(command.arg[2], command.arg[3]) / 2)
    return luaL_error(state, "rounded rectangle radius too large");
  uint32_t cost = 1024;
  if (op == DrawOp::FillScreen) cost = 320UL * 432UL;
  else if (widthIndex >= 0 && heightIndex >= 0)
    cost = static_cast<uint32_t>(command.arg[widthIndex] * command.arg[heightIndex]);
  else if (radiusIndex >= 0)
    cost = static_cast<uint32_t>(command.arg[radiusIndex] * command.arg[radiusIndex] * 4);
  else if (op == DrawOp::FillTriangle) cost = 320UL * 432UL;
  if (cost > 400000UL - runtime->_frameCost)
    return luaL_error(state, "frame drawing budget exceeded");
  runtime->_frameCost += cost;
  if (op == DrawOp::Progress && (command.arg[4] < 0 || command.arg[4] > 100))
    return luaL_error(state, "invalid progress value");
  if (op == DrawOp::Text && (command.arg[4] < 0 || command.arg[4] > 2 ||
                             command.arg[5] < 0 || command.arg[5] > 8))
    return luaL_error(state, "invalid text style");
  ++frame.count;
  return 0;
}

int AppRuntime::luaPlayTone(lua_State *state) {
  AppRuntime *runtime = self(state);
  const lua_Integer frequency = luaL_checkinteger(state, 1);
  const lua_Integer duration = luaL_checkinteger(state, 2);
  if (frequency < 100 || frequency > 4000 || duration < 10 || duration > 2000) {
    lua_pushboolean(state, false); lua_pushliteral(state, "tone out of range"); return 2;
  }
  if (runtime->_toneCount >= 8) {
    lua_pushboolean(state, false); lua_pushliteral(state, "tone queue full"); return 2;
  }
  const uint8_t tail = (runtime->_toneHead + runtime->_toneCount) % 8;
  runtime->_tones[tail] = {static_cast<uint16_t>(frequency), static_cast<uint16_t>(duration)};
  ++runtime->_toneCount;
  lua_pushboolean(state, true);
  return 1;
}

int AppRuntime::luaStopTones(lua_State *state) {
  AppRuntime *runtime = self(state);
  const int64_t started = esp_timer_get_time();
  runtime->clearTones();
  runtime->_callbackStartedUs += esp_timer_get_time() - started;
  return 0;
}
int AppRuntime::luaVolume(lua_State *state) {
  lua_pushinteger(state, self(state)->_audioSettings.volume()); return 1;
}
int AppRuntime::luaSetVolume(lua_State *state) {
  AppRuntime *runtime = self(state);
  const lua_Integer volume = luaL_checkinteger(state, 1);
  if (volume < 0 || volume > 10) {
    lua_pushboolean(state, false); lua_pushliteral(state, "volume out of range"); return 2;
  }
  const int64_t started = esp_timer_get_time();
  const bool success = runtime->_audioSettings.setVolume(static_cast<uint8_t>(volume));
  runtime->_callbackStartedUs += esp_timer_get_time() - started;
  lua_pushboolean(state, success);
  if (!success) { lua_pushliteral(state, "volume could not be saved"); return 2; }
  return 1;
}

void AppRuntime::clearTones() {
  _toneHead = _toneCount = 0;
  _toneRemaining = _tonePhase = _toneStep = 0;
  if (_audioReady) { _speaker.stop(); _audioReady = false; }
}

void AppRuntime::serviceTones() {
  if (_cancel || !_card.isMounted()) { clearTones(); return; }
  if (!_toneRemaining && !_toneCount) {
    if (_audioReady && millis() - _toneLastWriteMs >= 260) {
      _speaker.stop(); _audioReady = false;
    }
    return;
  }
  if (!_audioReady) {
    _audioReady = _speaker.start(16000);
    if (!_audioReady) { clearTones(); return; }
    _toneLastWriteMs = millis();
  }
  if (!_toneRemaining && _toneCount) {
    const Tone tone = _tones[_toneHead];
    _toneHead = (_toneHead + 1) % 8;
    --_toneCount;
    _toneRemaining = static_cast<uint32_t>(tone.durationMs) * 16;
    _tonePhase = 0;
    _toneStep = (static_cast<uint32_t>(tone.frequency) << 16) / 16000;
  }
  const uint16_t count = _toneRemaining > 256 ? 256 : static_cast<uint16_t>(_toneRemaining);
  for (uint16_t i = 0; i < count; ++i) {
    _toneBuffer[i] = (_tonePhase & 0x8000UL) ? 11000 : -11000;
    _tonePhase += _toneStep;
  }
  const size_t written = _speaker.tryWritePcm16(_toneBuffer, count);
  if (written) _toneLastWriteMs = millis();
  else if (millis() - _toneLastWriteMs > 1000) { clearTones(); return; }
  _toneRemaining -= written;
  _tonePhase -= _toneStep * (count - written);
}

void AppRuntime::registerApi() {
  luaL_requiref(_lua, "_G", luaopen_base, 1); lua_pop(_lua, 1);
  luaL_requiref(_lua, "table", luaopen_table, 1); lua_pop(_lua, 1);
  luaL_requiref(_lua, "string", luaopen_string, 1); lua_pop(_lua, 1);
  luaL_requiref(_lua, "math", luaopen_math, 1); lua_pop(_lua, 1);
  luaL_requiref(_lua, "utf8", luaopen_utf8, 1); lua_pop(_lua, 1);
  removeGlobal(_lua, "dofile");
  removeGlobal(_lua, "loadfile");
  removeGlobal(_lua, "load");
  removeGlobal(_lua, "collectgarbage");
  lua_getglobal(_lua, "string"); lua_pushnil(_lua); lua_setfield(_lua, -2, "dump"); lua_pop(_lua, 1);
  lua_newtable(_lua);
  lua_newtable(_lua);
  lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaShow, 1); lua_setfield(_lua, -2, "show");
  lua_setfield(_lua, -2, "ui");
  lua_newtable(_lua);
  lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaRead, 1); lua_setfield(_lua, -2, "read");
  lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaWrite, 1); lua_setfield(_lua, -2, "write");
  lua_setfield(_lua, -2, "storage");
  lua_newtable(_lua);
  lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaStatus, 1); lua_setfield(_lua, -2, "status");
  lua_setfield(_lua, -2, "device");
  lua_newtable(_lua);
  lua_pushcfunction(_lua, luaMillis); lua_setfield(_lua, -2, "millis");
  lua_setfield(_lua, -2, "time");
  if (_sdkVersion == 2) {
    lua_newtable(_lua);
    lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaBeginFrame, 1);
    lua_setfield(_lua, -2, "begin_frame");
    lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaPresent, 1);
    lua_setfield(_lua, -2, "present");
    struct DrawBinding { const char *name; DrawOp op; };
    static const DrawBinding bindings[] = {
      {"fillScreen", DrawOp::FillScreen}, {"fillRect", DrawOp::FillRect},
      {"drawRect", DrawOp::DrawRect}, {"drawLine", DrawOp::DrawLine},
      {"drawFastHLine", DrawOp::HLine}, {"drawFastVLine", DrawOp::VLine},
      {"drawCircle", DrawOp::DrawCircle}, {"fillCircle", DrawOp::FillCircle},
      {"drawRoundRect", DrawOp::DrawRoundRect}, {"fillRoundRect", DrawOp::FillRoundRect},
      {"fillTriangle", DrawOp::FillTriangle}, {"drawWideLine", DrawOp::DrawWideLine},
      {"drawSmoothCircle", DrawOp::DrawSmoothCircle}, {"fillSmoothCircle", DrawOp::FillSmoothCircle},
      {"drawSmoothRoundRect", DrawOp::DrawSmoothRoundRect},
      {"fillSmoothRoundRect", DrawOp::FillSmoothRoundRect},
      {"drawSmoothArc", DrawOp::DrawSmoothArc},
      {"drawString", DrawOp::Text}, {"drawNumber", DrawOp::Text}
    };
    for (const DrawBinding &binding : bindings) {
      lua_pushlightuserdata(_lua, this);
      lua_pushinteger(_lua, static_cast<lua_Integer>(binding.op));
      lua_pushcclosure(_lua, luaDraw, 2);
      lua_setfield(_lua, -2, binding.name);
    }
    lua_setfield(_lua, -2, "tft");
    lua_getfield(_lua, -1, "ui");
    for (const DrawBinding &binding : {
      DrawBinding{"button", DrawOp::Button}, DrawBinding{"progress", DrawOp::Progress}}) {
      lua_pushlightuserdata(_lua, this);
      lua_pushinteger(_lua, static_cast<lua_Integer>(binding.op));
      lua_pushcclosure(_lua, luaDraw, 2);
      lua_setfield(_lua, -2, binding.name);
    }
    lua_pop(_lua, 1);
    lua_newtable(_lua);
    lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaPlayTone, 1);
    lua_setfield(_lua, -2, "play_tone");
    lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaStopTones, 1);
    lua_setfield(_lua, -2, "stop_tones");
    lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaVolume, 1);
    lua_setfield(_lua, -2, "volume");
    lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaSetVolume, 1);
    lua_setfield(_lua, -2, "set_volume");
    lua_setfield(_lua, -2, "audio");
  }
  lua_setglobal(_lua, "esp");
}

void AppRuntime::setError(const String &message) {
  Serial.printf("[SD app] %s: %s\n", _id.c_str(), message.c_str());
  Scene scene{};
  copyText(scene.title, sizeof(scene.title), "App error");
  scene.count = 1;
  copyText(scene.rows[0].text, sizeof(scene.rows[0].text), message.c_str());
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    _scene = scene;
    ++_revision;
    xSemaphoreGive(_mutex);
  }
}

bool AppRuntime::invoke(const char *callback, const char *action,
                        uint32_t nowMs, uint32_t touchY) {
  lua_rawgeti(_lua, LUA_REGISTRYINDEX, _appRef);
  lua_getfield(_lua, -1, callback);
  if (lua_isnil(_lua, -1)) {
    lua_settop(_lua, 0);
    if (strcmp(callback, "on_start") == 0) { setError("Missing on_start callback"); return false; }
    return true;
  }
  if (!lua_isfunction(_lua, -1)) { lua_settop(_lua, 0); setError("Invalid app callback"); return false; }
  int arguments = 0;
  if (action) { lua_pushstring(_lua, action); arguments = 1; }
  else if (strcmp(callback, "on_tick") == 0) { lua_pushinteger(_lua, nowMs); arguments = 1; }
  else if (strcmp(callback, "on_touch") == 0) {
    lua_pushinteger(_lua, nowMs); lua_pushinteger(_lua, touchY); arguments = 2;
  }
  _hookCount = 0;
  _callbackStartedUs = esp_timer_get_time();
  lua_sethook(_lua, luaHook, LUA_MASKCOUNT, 1000);
  const int result = lua_pcall(_lua, arguments, 0, 0);
  lua_sethook(_lua, nullptr, 0, 0);
  if (result != LUA_OK || esp_timer_get_time() - _callbackStartedUs > CallbackMaxUs) {
    const char *error = lua_tostring(_lua, -1);
    setError(result != LUA_OK && error ? error : "App callback exceeded its time limit");
    lua_settop(_lua, 0);
    return false;
  }
  lua_settop(_lua, 0);
  return true;
}

void AppRuntime::taskMain(void *context) {
  AppRuntime *runtime = static_cast<AppRuntime *>(context);
  runtime->run();
  runtime->_stopped = true;
  vTaskDelay(portMAX_DELAY);
}

void AppRuntime::run() {
  AppPackage package = AppPackage::load(_card, _id);
  if (!package.valid) { setError(package.error); return; }
  _displayName = package.name;
  _sdkVersion = package.sdk;
  char *source = nullptr;
  size_t length = 0;
  String error;
  if (!AppPackage::readSource(_card, _id, source, length, error)) { setError(error); return; }
  _luaBytes = 0;
  _lua = lua_newstate(luaAlloc, this);
  if (!_lua) { heap_caps_free(source); setError("Lua memory unavailable"); return; }
  *static_cast<AppRuntime **>(lua_getextraspace(_lua)) = this;
  lua_setcstacklimit(_lua, 80);
  registerApi();
  const int loadResult = luaL_loadbufferx(_lua, source, length, _id.c_str(), "t");
  heap_caps_free(source);
  if (loadResult != LUA_OK) {
    const char *loadError = lua_tostring(_lua, -1);
    setError(loadError ? loadError : "Cannot load app source");
  } else {
    _hookCount = 0;
    _callbackStartedUs = esp_timer_get_time();
    lua_sethook(_lua, luaHook, LUA_MASKCOUNT, 1000);
    const int result = lua_pcall(_lua, 0, 1, 0);
    lua_sethook(_lua, nullptr, 0, 0);
    if (result != LUA_OK || esp_timer_get_time() - _callbackStartedUs > CallbackMaxUs ||
        !lua_istable(_lua, -1)) {
      if (result != LUA_OK) {
        const char *startupError = lua_tostring(_lua, -1);
        setError(startupError ? startupError : "App startup failed");
      }
      else if (esp_timer_get_time() - _callbackStartedUs > CallbackMaxUs)
        setError("App startup exceeded its time limit");
      else setError("App must return a callback table");
    } else {
      _appRef = luaL_ref(_lua, LUA_REGISTRYINDEX);
      if (invoke("on_start")) {
        uint32_t lastTick = millis();
        while (!_cancel && _card.isMounted()) {
          Action action{};
          const uint32_t waitMs = (_toneRemaining || _toneCount || _audioReady) ? 10 : TickIntervalMs;
          if (xQueueReceive(_actions, &action, pdMS_TO_TICKS(waitMs)) == pdTRUE) {
            if (action.touch) {
              if (!invoke("on_touch", nullptr, action.x, action.y)) break;
            } else if (!invoke("on_action", action.id)) break;
          }
          serviceTones();
          const uint32_t now = millis();
          if (now - lastTick >= TickIntervalMs) {
            lastTick = now;
            if (!invoke("on_tick", nullptr, now)) break;
          }
        }
      }
    }
  }
  clearTones();
  lua_close(_lua);
  _lua = nullptr;
  _appRef = LUA_NOREF;
}
