#include "AppRuntime.h"

#include <FS.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <math.h>

#include <algorithm>
#include <new>

#include "../../vendor_shims/lua/LuaConfig.h"

extern "C" {
#include "../../../third_party/lua/lua.h"
#include "../../../third_party/lua/lauxlib.h"
#include "../../../third_party/lua/lualib.h"
}

#include "../../core/FirmwareVersion.h"
#include "../../core/NaturalOrder.h"
#include "../../core/SpiBusLock.h"
#include "../../media/FileFormats.h"
#include "../../services/storage/PathPolicy.h"
#include "../../services/storage/SdCardController.h"
#include "../../services/audio/SpeakerController.h"
#include "../../services/audio/AudioSettings.h"
#include "../../services/audio/MicController.h"
#include "../../services/audio/WavFormat.h"
#include "AppNetWorker.h"
#include "AppPackage.h"
#include "AppText.h"

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

String baseName(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(slash + 1);
}

// Pushes the SDK's `false|nil, error` failure pair.
int pushFailure(lua_State *state, bool nilFirst, const char *error) {
  if (nilFirst) lua_pushnil(state); else lua_pushboolean(state, false);
  lua_pushstring(state, error);
  return 2;
}
}

AppRuntime::AppRuntime(SdCardController &card, SpeakerController &speaker,
                       AudioSettings &audioSettings, MicController &mic,
                       MediaCache &mediaCache)
  : _card(card), _speaker(speaker), _audioSettings(audioSettings), _mic(mic),
    _mediaCache(mediaCache) {}

bool AppRuntime::start(const String &id, const Status &initialStatus) {
  if (_task || !AppPackage::validId(id)) return false;
  if (_net) {
    if (!_net->stop()) return false;
    delete _net;
    _net = nullptr;
  }
  _id = id;
  _cancel = false;
  _stopped = false;
  _revision = 0;
  _scene = Scene{};
  _frameRevision = 0;
  _frameOpen = false;
  _frameCost = 0;
  _sdkVersion = 1;
  _filesPermission = false;
  _networkPermission = false;
  _hostCount = 0;
  _navigationPending = _inputProvided = false;
  _paused = false;
  _resumeRequested = false;
  _pausedFor = NavigationKind::None;
  _image = ImageLayer{};
  _image.barVisible = true;
  _imageStatus = ImageStatus{};
  _imageRevision = 0;
  _toneLastWriteMs = 0;
  _recActive = _recToFile = _playActive = _playFinished = false;
  _recError = _playError = String();
  _recBytes = _recPending = _recLevel = 0;
  _playWrittenBytes = 0;
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
                              WorkerPriority, &_task, 0) != pdPASS) {
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
  // A request may still be unwinding a connect; the host retries.
  if (_net) {
    if (!_net->stop()) return false;
    delete _net;
    _net = nullptr;
  }
  if (_task) { vTaskDelete(_task); _task = nullptr; }
  if (_actions) { vQueueDelete(_actions); _actions = nullptr; }
  if (_mutex) { vSemaphoreDelete(_mutex); _mutex = nullptr; }
  clearTones();
  if (_buildingFrame) { heap_caps_free(_buildingFrame); _buildingFrame = nullptr; }
  if (_publishedFrame) { heap_caps_free(_publishedFrame); _publishedFrame = nullptr; }
  releaseBitmaps();
  _id = String();
  _displayName = String();
  _downloads = String();
  return true;
}

bool AppRuntime::sendAction(const char *id) {
  if (!_actions || _cancel || _stopped || !id) return false;
  Action action{};
  action.kind = ActionKind::Button;
  copyText(action.id, sizeof(action.id), id);
  return xQueueSend(_actions, &action, 0) == pdTRUE;
}

bool AppRuntime::sendTouch(int16_t x, int16_t y) {
  if (!_actions || _cancel || _stopped) return false;
  Action action{};
  action.x = x; action.y = y; action.kind = ActionKind::Touch;
  return xQueueSend(_actions, &action, 0) == pdTRUE;
}

bool AppRuntime::sendTouchEnd(int16_t x, int16_t y, uint32_t heldMs) {
  if (!_actions || _cancel || _stopped) return false;
  Action action{};
  action.x = x; action.y = y; action.heldMs = heldMs; action.kind = ActionKind::TouchEnd;
  return xQueueSend(_actions, &action, 0) == pdTRUE;
}

bool AppRuntime::takeNavigation(Navigation &navigation) {
  if (!_paused || _stopped || !_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
  const bool ready = _navigationPending;
  if (ready) { navigation = _navigation; _navigationPending = false; }
  xSemaphoreGive(_mutex);
  return ready;
}

void AppRuntime::resume(const char *input) {
  if (!_mutex) return;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    _inputProvided = input != nullptr;
    copyText(_input, sizeof(_input), input);
    xSemaphoreGive(_mutex);
  }
  _resumeRequested = true;
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

bool AppRuntime::takeImage(ImageLayer &layer, uint32_t &revision) {
  if (!_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return false;
  const bool changed = _imageRevision != revision;
  if (changed) { layer = _image; revision = _imageRevision; }
  xSemaphoreGive(_mutex);
  return changed;
}

void AppRuntime::publishImage(const ImageStatus &status) {
  if (!_mutex || xSemaphoreTake(_mutex, 0) != pdTRUE) return;
  _imageStatus = status;
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
      callbackClockUs() - runtime->_callbackStartedUs > CallbackMaxUs) {
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
  if (!SpiBusLock::lock(AppPackage::SdLockTimeout)) { error = "SD busy"; return false; }
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
    if (!SpiBusLock::lock(AppPackage::SdLockTimeout)) { error = "SD busy"; break; }
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

bool AppRuntime::ensureDataDirectory() {
  const String directory = dataDirectory(_id);
  return (SD.exists("/.system") || SD.mkdir("/.system")) &&
         (SD.exists("/.system/appdata") || SD.mkdir("/.system/appdata")) &&
         (SD.exists(directory.c_str()) || SD.mkdir(directory.c_str()));
}

bool AppRuntime::writeData(const char *name, const char *data, size_t length, String &error) {
  if (!AppPackage::validDataName(name) || length > AppPackage::MaxDataBytes) {
    error = "Invalid file name or size"; return false;
  }
  if (!_card.isMounted()) { error = "SD card unavailable"; return false; }
  const String path = dataPath(_id, name);
  const String temp = path + ".tmp";
  if (!SpiBusLock::lock(AppPackage::SdLockTimeout)) { error = "SD busy"; return false; }
  bool ready = ensureDataDirectory();
  if (ready) SD.remove(temp.c_str());
  File file = ready ? SD.open(temp.c_str(), FILE_WRITE) : File();
  SpiBusLock::unlock();
  if (!file) { error = "Cannot open app data"; return false; }
  size_t written = 0;
  while (written < length && !_cancel && _card.isMounted()) {
    if (!SpiBusLock::lock(AppPackage::SdLockTimeout)) { error = "SD busy"; break; }
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
  if (success && SpiBusLock::lock(AppPackage::SdLockTimeout)) {
    if (SD.exists(path.c_str())) SD.remove(path.c_str());
    success = SD.rename(temp.c_str(), path.c_str());
    SpiBusLock::unlock();
  } else success = false;
  if (!success) {
    if (SpiBusLock::lock(AppPackage::SdLockTimeout)) { SD.remove(temp.c_str()); SpiBusLock::unlock(); }
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
  const int64_t started = callbackClockUs();
  if (!runtime->readData(name, data, length, error)) {
    runtime->_callbackStartedUs += callbackClockUs() - started;
    lua_pushnil(state); lua_pushstring(state, error.c_str()); return 2;
  }
  runtime->_callbackStartedUs += callbackClockUs() - started;
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
  const int64_t started = callbackClockUs();
  const bool success = runtime->writeData(name, data, length, error);
  runtime->_callbackStartedUs += callbackClockUs() - started;
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
  if (runtime->_sdkVersion < 2 || !runtime->_buildingFrame)
    return luaL_error(state, "SDK 2+ frame unavailable");
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
  else if (op == DrawOp::FillTriangle) {
    // The bounding box, capped at the canvas.
    const int32_t *a = command.arg;
    const int64_t width = max(a[0], max(a[2], a[4])) - min(a[0], min(a[2], a[4])) + 1;
    const int64_t height = max(a[1], max(a[3], a[5])) - min(a[1], min(a[3], a[5])) + 1;
    cost = static_cast<uint32_t>(min<int64_t>(width * height, 320LL * 432LL));
  }
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
  if (runtime->_recActive || runtime->_playActive) {
    lua_pushboolean(state, false); lua_pushliteral(state, "audio busy"); return 2;
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
  const int64_t started = callbackClockUs();
  runtime->clearTones();
  runtime->_callbackStartedUs += callbackClockUs() - started;
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
  const int64_t started = callbackClockUs();
  const bool success = runtime->_audioSettings.setVolume(static_cast<uint8_t>(volume));
  runtime->_callbackStartedUs += callbackClockUs() - started;
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

int AppRuntime::luaStorageSize(lua_State *state) {
  AppRuntime *runtime = self(state);
  size_t nameLength = 0;
  const char *name = luaL_checklstring(state, 1, &nameLength);
  if (nameLength > 64 || !AppPackage::validDataName(name)) {
    lua_pushnil(state); lua_pushliteral(state, "Invalid file name"); return 2;
  }
  if (!runtime->_card.isMounted()) {
    lua_pushnil(state); lua_pushliteral(state, "SD card unavailable"); return 2;
  }
  const int64_t started = callbackClockUs();
  bool found = false;
  size_t size = 0;
  const bool locked = SpiBusLock::lock(AppPackage::SdLockTimeout);
  if (locked) {
    File file = SD.open(dataPath(runtime->_id, name).c_str(), FILE_READ);
    found = file && !file.isDirectory();
    size = found ? static_cast<size_t>(file.size()) : 0;
    if (file) file.close();
    SpiBusLock::unlock();
  }
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!locked) { lua_pushnil(state); lua_pushliteral(state, "SD busy"); return 2; }
  if (!found) { lua_pushnil(state); lua_pushliteral(state, "File not found"); return 2; }
  lua_pushinteger(state, static_cast<lua_Integer>(size));
  return 1;
}

int AppRuntime::luaMicStatus(lua_State *state) {
  AppRuntime *runtime = self(state);
  if (!runtime->_mic.isReady()) runtime->_mic.begin();
  const uint64_t bytesPerSecond = static_cast<uint64_t>(runtime->_mic.sampleRate()) * 2;
  lua_createtable(state, 0, 7);
  lua_pushboolean(state, runtime->_mic.isReady()); lua_setfield(state, -2, "ready");
  lua_pushboolean(state, runtime->_mic.isPowered()); lua_setfield(state, -2, "powered");
  lua_pushboolean(state, runtime->_recActive); lua_setfield(state, -2, "recording");
  lua_pushinteger(state, static_cast<lua_Integer>(
      static_cast<uint64_t>(runtime->_recBytes) * 1000 / bytesPerSecond));
  lua_setfield(state, -2, "elapsed_ms");
  lua_pushinteger(state, runtime->_recBytes); lua_setfield(state, -2, "bytes");
  lua_pushinteger(state, runtime->_recActive ? runtime->_recLevel : 0);
  lua_setfield(state, -2, "level");
  if (!runtime->_recError.isEmpty()) {
    lua_pushstring(state, runtime->_recError.c_str()); lua_setfield(state, -2, "error");
  }
  return 1;
}

int AppRuntime::luaMicStart(lua_State *state) {
  AppRuntime *runtime = self(state);
  const char *name = nullptr;
  if (!lua_isnoneornil(state, 1)) {
    size_t nameLength = 0;
    name = luaL_checklstring(state, 1, &nameLength);
    if (nameLength > 64 || memchr(name, 0, nameLength)) {
      lua_pushboolean(state, false); lua_pushliteral(state, "Invalid file name"); return 2;
    }
  }
  const lua_Integer seconds = luaL_optinteger(state, 2, 600);
  if (seconds < 1 || seconds > 600) {
    lua_pushboolean(state, false); lua_pushliteral(state, "duration out of range"); return 2;
  }
  String error;
  const int64_t started = callbackClockUs();
  const bool success = runtime->startRecording(name, static_cast<uint32_t>(seconds), error);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  lua_pushboolean(state, success);
  if (!success) { lua_pushstring(state, error.c_str()); return 2; }
  return 1;
}

int AppRuntime::luaMicStop(lua_State *state) {
  AppRuntime *runtime = self(state);
  if (!runtime->_recActive) {
    lua_pushboolean(state, false); lua_pushliteral(state, "Not recording"); return 2;
  }
  uint32_t bytes = 0;
  const int64_t started = callbackClockUs();
  const bool success = runtime->finishRecording("", &bytes);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  lua_pushboolean(state, success);
  if (!success) { lua_pushstring(state, runtime->_recError.c_str()); return 2; }
  lua_pushinteger(state, bytes);
  return 2;
}

int AppRuntime::luaPlayFile(lua_State *state) {
  AppRuntime *runtime = self(state);
  size_t nameLength = 0;
  const char *name = luaL_checklstring(state, 1, &nameLength);
  if (nameLength > 64 || memchr(name, 0, nameLength)) {
    lua_pushboolean(state, false); lua_pushliteral(state, "Invalid file name"); return 2;
  }
  String error;
  const int64_t started = callbackClockUs();
  const bool success = runtime->startPlayback(name, error);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  lua_pushboolean(state, success);
  if (!success) { lua_pushstring(state, error.c_str()); return 2; }
  return 1;
}

int AppRuntime::luaStopFile(lua_State *state) {
  AppRuntime *runtime = self(state);
  const int64_t started = callbackClockUs();
  runtime->finishPlayback(nullptr, false);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  return 0;
}

int AppRuntime::luaPlaybackStatus(lua_State *state) {
  AppRuntime *runtime = self(state);
  const uint64_t bytesPerSecond = static_cast<uint64_t>(runtime->_playSampleRate) * 2;
  lua_createtable(state, 0, 4);
  lua_pushboolean(state, runtime->_playActive); lua_setfield(state, -2, "playing");
  lua_pushboolean(state, runtime->_playFinished); lua_setfield(state, -2, "finished");
  lua_pushinteger(state, static_cast<lua_Integer>(
      static_cast<uint64_t>(runtime->_playWrittenBytes) * 1000 / bytesPerSecond));
  lua_setfield(state, -2, "elapsed_ms");
  if (!runtime->_playError.isEmpty()) {
    lua_pushstring(state, runtime->_playError.c_str()); lua_setfield(state, -2, "error");
  }
  return 1;
}

bool AppRuntime::startRecording(const char *name, uint32_t maxSeconds, String &error) {
  if (_recActive) { error = "Already recording"; return false; }
  if (name && !AppPackage::validDataName(name)) { error = "Invalid file name"; return false; }
  if (name && !_card.isMounted()) { error = "SD card unavailable"; return false; }
  if (!_mic.isReady() && !_mic.begin()) { error = _mic.lastError(); return false; }
  if (!_mic.isPowered()) { error = "Turn mic power switch on"; return false; }
  if (_mic.isRecording()) { error = "Microphone busy"; return false; }
  // The microphone and speaker share clock pins; release the speaker first.
  finishPlayback(nullptr, false);
  clearTones();

  const uint32_t sampleRate = _mic.sampleRate();
  if (name) {
    _recPath = dataPath(_id, name);
    _recTempPath = _recPath + ".tmp";
    if (!SpiBusLock::lock(AppPackage::SdLockTimeout)) { error = "SD busy"; return false; }
    const bool ready = ensureDataDirectory();
    if (ready) SD.remove(_recTempPath.c_str());
    _recFile = ready ? SD.open(_recTempPath.c_str(), FILE_WRITE) : File();
    uint8_t header[WavFormat::HeaderBytes];
    WavFormat::makeHeader(header, sampleRate, 0);
    const bool wrote = _recFile && _recFile.write(header, sizeof(header)) == sizeof(header);
    if (_recFile && !wrote) { _recFile.close(); SD.remove(_recTempPath.c_str()); }
    SpiBusLock::unlock();
    if (!wrote) { _recFile = File(); error = "Cannot create recording"; return false; }
  }
  if (!_mic.start()) {
    error = _mic.lastError();
    if (name && SpiBusLock::lock(portMAX_DELAY)) {
      _recFile.close();
      SD.remove(_recTempPath.c_str());
      SpiBusLock::unlock();
    }
    _recFile = File();
    return false;
  }
  _recToFile = name != nullptr;
  _recBytes = _recPending = _recFlushedBytes = 0;
  _recMaxBytes = maxSeconds * sampleRate * 2;
  _recError = String();
  _recLevel = 0;
  _recLevelMs = millis();
  _recActive = true;
  return true;
}

bool AppRuntime::finishRecording(const char *reason, uint32_t *bytes) {
  if (!_recActive) return false;
  _recActive = false;
  _mic.stop();
  bool saved = true;
  if (_recToFile) {
    const bool mounted = _card.isMounted();
    saved = false;
    if (SpiBusLock::lock(portMAX_DELAY)) {
      if (mounted && _recBytes > 0) {
        uint8_t header[WavFormat::HeaderBytes];
        WavFormat::makeHeader(header, _mic.sampleRate(), _recBytes);
        saved = _recFile.seek(0) && _recFile.write(header, sizeof(header)) == sizeof(header);
        _recFile.flush();
      }
      _recFile.close();
      if (mounted) {
        if (saved) {
          if (SD.exists(_recPath.c_str())) SD.remove(_recPath.c_str());
          saved = SD.rename(_recTempPath.c_str(), _recPath.c_str());
        }
        if (!saved) SD.remove(_recTempPath.c_str());
      }
      SpiBusLock::unlock();
    }
    _recFile = File();
  }
  _recPending = 0;
  _recLevel = 0;
  if (reason && *reason) _recError = reason;
  else if (!saved) _recError = _recBytes == 0 ? "Recording was empty" : "Recording could not be saved";
  else _recError = String();
  if (bytes) *bytes = _recBytes;
  return saved;
}

void AppRuntime::serviceRecording() {
  if (!_recActive) return;
  if (_recToFile && !_card.isMounted()) { finishRecording("SD card removed"); return; }
  if (!_mic.isPowered()) { finishRecording("Mic switch turned off"); return; }

  // Drain what DMA already captured; never wait for samples here.
  const size_t capacity = sizeof(_recBuffer) / sizeof(_recBuffer[0]);
  const size_t room = (_recMaxBytes - _recBytes) / 2;
  const size_t want = room < capacity ? room : capacity;
  size_t total = _recPending;
  int32_t peak = 0;
  while (total < want) {
    const size_t count = _mic.readPcm16(_recBuffer + total, want - total, 0);
    if (!count) break;
    for (size_t i = 0; i < count; ++i) {
      const int32_t magnitude = abs(static_cast<int32_t>(_recBuffer[total + i]));
      if (magnitude > peak) peak = magnitude;
    }
    total += count;
  }

  // Peak level on a 60 dB scale with a roughly 0.8 s fall time.
  const uint32_t now = millis();
  const uint32_t decay = (now - _recLevelMs) / 8;
  _recLevelMs = now;
  _recLevel = _recLevel > decay ? static_cast<uint8_t>(_recLevel - decay) : 0;
  if (peak > 0) {
    float level = (20.0f * log10f(static_cast<float>(peak) / 32768.0f) + 60.0f) * (100.0f / 60.0f);
    if (level < 0.0f) level = 0.0f;
    if (level > 100.0f) level = 100.0f;
    if (level > _recLevel) _recLevel = static_cast<uint8_t>(level + 0.5f);
  }

  if (total) {
    if (!_recToFile) {
      _recBytes += total * 2;
      total = 0;
    } else if (SpiBusLock::lock(pdMS_TO_TICKS(20))) {
      const size_t bytesToWrite = total * 2;
      const size_t written = _recFile.write(reinterpret_cast<const uint8_t *>(_recBuffer), bytesToWrite);
      if (written == bytesToWrite && _recBytes + written - _recFlushedBytes >= 8192) {
        _recFile.flush();
        _recFlushedBytes = _recBytes + written;
      }
      SpiBusLock::unlock();
      if (written != bytesToWrite) { finishRecording("SD write failed"); return; }
      _recBytes += written;
      total = 0;
    }
    // A busy SD bus keeps samples buffered for the next pass.
    _recPending = static_cast<uint16_t>(total);
  }
  if (_recBytes >= _recMaxBytes) finishRecording("Recording limit reached");
}

bool AppRuntime::startPlayback(const char *name, String &error) {
  if (!AppPackage::validDataName(name)) { error = "Invalid file name"; return false; }
  if (!_card.isMounted()) { error = "SD card unavailable"; return false; }
  if (_recActive || _mic.isRecording()) { error = "Microphone is recording"; return false; }
  finishPlayback(nullptr, false);
  clearTones();

  uint8_t header[WavFormat::HeaderBytes];
  uint32_t sampleRate = 0, dataBytes = 0;
  size_t fileSize = 0;
  if (!SpiBusLock::lock(AppPackage::SdLockTimeout)) { error = "SD busy"; return false; }
  _playFile = SD.open(dataPath(_id, name).c_str(), FILE_READ);
  bool opened = _playFile && !_playFile.isDirectory();
  if (opened) {
    fileSize = static_cast<size_t>(_playFile.size());
    opened = _playFile.read(header, sizeof(header)) == sizeof(header);
  }
  if (!opened && _playFile) _playFile.close();
  SpiBusLock::unlock();
  if (!opened) { _playFile = File(); error = "File not found"; return false; }

  if (!WavFormat::parseHeader(header, sampleRate, dataBytes) ||
      sampleRate < 8000 || sampleRate > 44100) {
    error = "Unsupported WAV format";
  } else {
    // Trust the file size over a header that was never finalized.
    const size_t available = fileSize - sizeof(header);
    if (dataBytes > available) dataBytes = static_cast<uint32_t>(available);
    dataBytes &= ~1U;
    if (!dataBytes) error = "Unsupported WAV format";
  }
  if (error.isEmpty() && !_speaker.start(sampleRate)) error = _speaker.lastError();
  if (!error.isEmpty()) {
    if (SpiBusLock::lock(portMAX_DELAY)) { _playFile.close(); SpiBusLock::unlock(); }
    _playFile = File();
    return false;
  }
  _playSampleRate = sampleRate;
  _playRemainingBytes = dataBytes;
  _playWrittenBytes = 0;
  _playPendingSamples = _playPendingOffset = 0;
  _playLastWriteMs = millis();
  _playEndedMs = 0;
  _playError = String();
  _playFinished = false;
  _playActive = true;
  return true;
}

void AppRuntime::finishPlayback(const char *error, bool finished) {
  if (!_playActive) return;
  _playActive = false;
  if (SpiBusLock::lock(portMAX_DELAY)) { _playFile.close(); SpiBusLock::unlock(); }
  _playFile = File();
  _speaker.stop();
  _playPendingSamples = _playPendingOffset = 0;
  _playFinished = finished;
  _playError = error ? String(error) : String();
}

void AppRuntime::servicePlayback() {
  if (!_playActive) return;
  if (!_card.isMounted()) { finishPlayback("SD card removed", false); return; }
  const size_t capacity = sizeof(_toneBuffer);
  for (int pass = 0; pass < 4; ++pass) {
    const uint32_t now = millis();
    if (_playPendingOffset >= _playPendingSamples) {
      _playPendingOffset = _playPendingSamples = 0;
      if (!_playRemainingBytes) {
        // Let the speaker's DMA queue (about 200 ms) play out before muting.
        if (!_playEndedMs) _playEndedMs = now ? now : 1;
        if (now - _playEndedMs >= 300) finishPlayback(nullptr, true);
        return;
      }
      const size_t chunk = _playRemainingBytes < capacity ? _playRemainingBytes : capacity;
      if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return;
      size_t count = _playFile.read(reinterpret_cast<uint8_t *>(_toneBuffer), chunk);
      SpiBusLock::unlock();
      count &= ~static_cast<size_t>(1);
      if (!count) { _playRemainingBytes = 0; return; }
      _playPendingSamples = static_cast<uint16_t>(count / 2);
      _playRemainingBytes -= count;
    }
    const size_t written = _speaker.tryWritePcm16(_toneBuffer + _playPendingOffset,
                                                  _playPendingSamples - _playPendingOffset);
    if (!written) {
      if (now - _playLastWriteMs > 1000) finishPlayback("Speaker stalled", false);
      return;
    }
    _playPendingOffset += written;
    _playWrittenBytes += written * 2;
    _playLastWriteMs = now;
  }
}

int AppRuntime::luaListCell(lua_State *state) {
  AppRuntime *runtime = self(state);
  if (!runtime->_frameOpen || !runtime->_buildingFrame)
    return luaL_error(state, "begin_frame required");
  Frame &frame = *runtime->_buildingFrame;
  if (frame.count >= MaxDrawCommands) return luaL_error(state, "too many draw commands");
  static const char *const icons[] = {
    "none", "folder", "file", "image", "audio", "video", "text", nullptr};
  static const char *const actions[] = {"none", "more", "close", nullptr};
  const lua_Integer y = luaL_checkinteger(state, 1);
  size_t length = 0;
  const char *label = luaL_checklstring(state, 2, &length);
  const int icon = luaL_checkoption(state, 3, "none", icons);
  const int action = luaL_checkoption(state, 4, "none", actions);
  if (y < -512 || y > 1024) return luaL_error(state, "draw argument out of range");
  if (!length || length > 96 || memchr(label, 0, length))
    return luaL_error(state, "invalid draw text");
  const uint32_t cost = 320UL * 40UL;
  if (cost > 400000UL - runtime->_frameCost)
    return luaL_error(state, "frame drawing budget exceeded");
  runtime->_frameCost += cost;
  DrawCommand &command = frame.commands[frame.count++];
  command = DrawCommand{};
  command.op = DrawOp::ListCell;
  command.arg[0] = static_cast<int32_t>(y);
  command.arg[1] = icon;
  command.arg[2] = action;
  copyText(command.text, sizeof(command.text), label);
  return 0;
}

int AppRuntime::luaPrompt(lua_State *state) {
  AppRuntime *runtime = self(state);
  size_t titleLength = 0, initialLength = 0;
  const char *title = luaL_checklstring(state, 1, &titleLength);
  const char *initial = luaL_optlstring(state, 2, "", &initialLength);
  const size_t initialLimit = runtime->_sdkVersion >= 6 ? MaxPromptBytes : 255;
  if (!titleLength || titleLength > 40 || memchr(title, 0, titleLength) ||
      initialLength > initialLimit || memchr(initial, 0, initialLength))
    return luaL_error(state, "invalid prompt text");
  bool mask = false;
  if (runtime->_sdkVersion >= 6 && !lua_isnoneornil(state, 3)) {
    luaL_checktype(state, 3, LUA_TTABLE);
    lua_getfield(state, 3, "mask");
    mask = lua_toboolean(state, -1);
    lua_pop(state, 1);
  }
  if (runtime->_navigationPending) return pushFailure(state, false, "Navigation pending");
  if (xSemaphoreTake(runtime->_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return pushFailure(state, false, "Host busy");
  runtime->_navigation = Navigation{};
  runtime->_navigation.kind = NavigationKind::Prompt;
  runtime->_navigation.mask = mask;
  copyText(runtime->_navigation.title, sizeof(runtime->_navigation.title), title);
  copyText(runtime->_navigation.initial, sizeof(runtime->_navigation.initial), initial);
  runtime->_navigationPending = true;
  xSemaphoreGive(runtime->_mutex);
  lua_pushboolean(state, true);
  return 1;
}

void AppRuntime::pauseForNavigation() {
  // The screen about to cover the app may need the speaker or microphone.
  finishRecording("");
  finishPlayback(nullptr, false);
  clearTones();
  // The covering screen may be the built-in viewer, which shares the pipeline.
  closeImage();
  _pausedFor = _navigation.kind;
  _paused = true;
}

bool AppRuntime::resumeFromNavigation() {
  char text[sizeof(_input)] = {};
  bool provided = false;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    provided = _inputProvided;
    copyText(text, sizeof(text), _input);
    // Earlier SDKs documented a 255-byte result; cut on a character boundary.
    const size_t limit = _sdkVersion >= 6 ? MaxPromptBytes : 255;
    text[AppText::utf8Prefix(text, strlen(text), limit)] = 0;
    // A request the host never took (push failed) is dropped with the pause.
    _navigationPending = false;
    xSemaphoreGive(_mutex);
  }
  const bool prompt = _pausedFor == NavigationKind::Prompt;
  _pausedFor = NavigationKind::None;
  _resumeRequested = false;
  _paused = false;
  xQueueReset(_actions);
  if (prompt) {
    CallArgs arguments;
    if (provided) arguments.text = text; else arguments.nil = true;
    if (!invoke("on_input", arguments)) return false;
  }
  return invoke("on_resume");
}

bool AppRuntime::checkFsPath(lua_State *state, int index, bool allowRoot, bool mutation,
                             bool nilOnError, String &path) {
  size_t length = 0;
  const char *value = luaL_checklstring(state, index, &length);
  const char *error = nullptr;
  if (memchr(value, 0, length) || !PathPolicy::appPath(value, length, allowRoot, path))
    error = "Invalid path";
  else if (mutation && !PathPolicy::appMutable(path)) error = "Path is read-only";
  else if (!_card.isMounted()) error = "SD card unavailable";
  if (!error) return true;
  pushFailure(state, nilOnError, error);
  return false;
}

void AppRuntime::pushFsEntry(lua_State *state, const char *name, bool directory,
                             uint32_t size, uint32_t modified, uint8_t formatValue) {
  const FileFormats::Format format = static_cast<FileFormats::Format>(formatValue);
  const char *kind = "other";
  if (directory) kind = "folder";
  else if (!FileFormats::isInternal(format)) {
    switch (FileFormats::group(format)) {
      case FileFormats::Group::Image: kind = "image"; break;
      case FileFormats::Group::Audio: kind = "audio"; break;
      case FileFormats::Group::Video: kind = "video"; break;
      case FileFormats::Group::Text: kind = "text"; break;
      default: break;
    }
  }
  lua_createtable(state, 0, 6);
  lua_pushstring(state, name); lua_setfield(state, -2, "name");
  lua_pushboolean(state, directory); lua_setfield(state, -2, "dir");
  lua_pushinteger(state, size); lua_setfield(state, -2, "size");
  lua_pushinteger(state, modified); lua_setfield(state, -2, "modified");
  lua_pushstring(state, kind); lua_setfield(state, -2, "kind");
  lua_pushboolean(state, !directory && FileFormats::isOpenable(format));
  lua_setfield(state, -2, "openable");
}

bool AppRuntime::startScan(const String &path, String &error) {
  closeScan(false);
  if (!SpiBusLock::lock(AppPackage::SdLockTimeout)) { error = "SD busy"; return false; }
  _fsDirectory = SD.open(path.c_str());
  const bool valid = _fsDirectory && _fsDirectory.isDirectory();
  if (!valid && _fsDirectory) _fsDirectory.close();
  SpiBusLock::unlock();
  if (!valid) { _fsDirectory = File(); error = "Folder not found"; return false; }
  _fsScanPath = path;
  _fsScanError = String();
  _fsScanning = true;
  return true;
}

void AppRuntime::closeScan(bool keepListing) {
  if (_fsDirectory) {
    if (SpiBusLock::lock(portMAX_DELAY)) { _fsDirectory.close(); SpiBusLock::unlock(); }
    _fsDirectory = File();
  }
  _fsScanning = false;
  if (keepListing) return;
  if (_fsEntries) { heap_caps_free(_fsEntries); _fsEntries = nullptr; }
  if (_fsNames) { heap_caps_free(_fsNames); _fsNames = nullptr; }
  _fsCount = _fsCapacity = 0;
  _fsNamesUsed = _fsNamesCapacity = 0;
  _fsScanReady = _fsTruncated = false;
}

bool AppRuntime::appendScanEntry(const String &name, bool directory, uint32_t size,
                                 uint32_t modified) {
  if (_fsCount == _fsCapacity) {
    const uint32_t doubled = _fsCapacity * 2UL;
    const uint16_t capacity = !_fsCapacity ? 64 :
        static_cast<uint16_t>(doubled < MaxFsEntries ? doubled : MaxFsEntries);
    FsEntry *entries = static_cast<FsEntry *>(heap_caps_realloc(
        _fsEntries, capacity * sizeof(FsEntry), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!entries) return false;
    _fsEntries = entries;
    _fsCapacity = capacity;
  }
  const uint32_t bytes = name.length() + 1;
  if (_fsNamesUsed + bytes > _fsNamesCapacity) {
    uint32_t capacity = _fsNamesCapacity ? _fsNamesCapacity * 2 : 4096;
    while (capacity < _fsNamesUsed + bytes) capacity *= 2;
    char *names = static_cast<char *>(heap_caps_realloc(
        _fsNames, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!names) return false;
    _fsNames = names;
    _fsNamesCapacity = capacity;
  }
  memcpy(_fsNames + _fsNamesUsed, name.c_str(), bytes);
  FsEntry &entry = _fsEntries[_fsCount++];
  entry.nameOffset = _fsNamesUsed;
  entry.size = size;
  entry.modified = modified;
  entry.format = static_cast<uint8_t>(directory ? FileFormats::Format::Unknown
                                                 : FileFormats::fromPath(name));
  entry.directory = directory;
  _fsNamesUsed += bytes;
  return true;
}

void AppRuntime::serviceScan() {
  if (!_fsScanning) return;
  // One directory entry per bus transaction, for a bounded slice of each pass.
  const int64_t deadline = esp_timer_get_time() + 8000;
  bool finished = false;
  while (!finished && !_cancel && esp_timer_get_time() < deadline) {
    if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return;
    File entry = _fsDirectory.openNextFile();
    const bool found = static_cast<bool>(entry);
    String name;
    bool directory = false;
    uint32_t size = 0, modified = 0;
    if (found) {
      name = baseName(entry.name());
      directory = entry.isDirectory();
      size = directory ? 0 : static_cast<uint32_t>(entry.size());
      modified = static_cast<uint32_t>(entry.getLastWrite());
      entry.close();
    }
    SpiBusLock::unlock();
    taskYIELD();
    if (!found) { finished = true; break; }
    // Skip what no SDK path could address afterwards.
    if (PathPolicy::hiddenName(name) ||
        _fsScanPath.length() + 1 + name.length() > PathPolicy::MaxPathBytes ||
        (!directory && FileFormats::isInternal(FileFormats::fromPath(name)))) continue;
    if (_fsCount >= MaxFsEntries) { _fsTruncated = true; finished = true; break; }
    if (!appendScanEntry(name, directory, size, modified)) {
      closeScan(false);
      _fsScanError = "App memory unavailable";
      return;
    }
  }
  if (!finished) return;
  closeScan(true);
  const char *names = _fsNames;
  std::sort(_fsEntries, _fsEntries + _fsCount, [names](const FsEntry &left, const FsEntry &right) {
    if (left.directory != right.directory) return left.directory;
    const char *l = names + left.nameOffset;
    const char *r = names + right.nameOffset;
    return NaturalOrder::compare(l, strlen(l), r, strlen(r)) < 0;
  });
  _fsScanReady = true;
}

bool AppRuntime::startJob(FsJob job, const String &source, const String &target, String &error) {
  if (_fsJob != FsJob::None) { error = "File job active"; return false; }
  if (!SpiBusLock::lock(AppPackage::SdLockTimeout)) { error = "SD busy"; return false; }
  File file = SD.open(source.c_str(), FILE_READ);
  const bool exists = static_cast<bool>(file);
  const bool directory = exists && file.isDirectory();
  if (file) file.close();
  const bool targetExists = job == FsJob::Rename && SD.exists(target.c_str());
  SpiBusLock::unlock();
  if (!exists) { error = "File not found"; return false; }
  if (targetExists) { error = "Target exists"; return false; }
  // Any mutation makes the current listing stale; the app scans again.
  closeScan(false);
  _fsJob = _fsLastJob = job;
  _fsJobSource = source;
  _fsJobTarget = target;
  _fsJobError = String();
  _fsJobDone = 0;
  _fsJobFinished = _fsJobCancel = false;
  _fsDepth = 0;
  if (!directory) {
    // Sidecar caches are bound to the source file and must not outlive it.
    _mediaCache.beginCleanup(source, _fsCleanup);
    _fsPhase = FsPhase::SourceCleanup;
  } else if (job == FsJob::Remove) {
    _fsStack[0] = source;
    _fsDepth = 1;
    _fsPhase = FsPhase::Tree;
  } else {
    _fsPhase = FsPhase::Commit;
  }
  return true;
}

void AppRuntime::finishJob(const char *error) {
  _fsJob = FsJob::None;
  _fsJobError = error ? String(error) : String();
  _fsJobFinished = error == nullptr;
  _fsJobCancel = false;
  for (uint8_t i = 0; i < MaxFsDepth; ++i) _fsStack[i] = String();
  _fsDepth = 0;
  _fsCleanup = MediaCache::Cleanup();
}

// Runs one bounded SD transaction. Returns false when the job ended or the
// bus was busy, so the caller leaves the rest for the next pass.
bool AppRuntime::stepJob() {
  if (_fsJobCancel) { finishJob("Cancelled"); return false; }
  if (_fsPhase == FsPhase::SourceCleanup || _fsPhase == FsPhase::TargetCleanup) {
    const MediaCache::Result result = _mediaCache.cleanupStep(_fsCleanup);
    if (result == MediaCache::Result::Busy) return false;
    if (result == MediaCache::Result::More) return true;
    if (result != MediaCache::Result::Complete) { finishJob("Cache cleanup failed"); return false; }
    if (_fsPhase == FsPhase::SourceCleanup && _fsJob == FsJob::Rename) {
      // Stale sidecars left under the new name would describe another file.
      _mediaCache.beginCleanup(_fsJobTarget, _fsCleanup);
      _fsPhase = FsPhase::TargetCleanup;
    } else {
      _fsPhase = FsPhase::Commit;
    }
    return true;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return false;
  if (_fsPhase == FsPhase::Commit) {
    const bool done = _fsJob == FsJob::Rename
        ? !SD.exists(_fsJobTarget.c_str()) && SD.rename(_fsJobSource.c_str(), _fsJobTarget.c_str())
        : SD.remove(_fsJobSource.c_str());
    SpiBusLock::unlock();
    if (done) ++_fsJobDone;
    finishJob(done ? nullptr : (_fsJob == FsJob::Rename ? "Rename failed" : "Delete failed"));
    return false;
  }
  // Tree delete: remove the first child of the deepest folder, descend into a
  // child folder, or remove the folder itself once it is empty.
  const String top = _fsStack[_fsDepth - 1];
  File directory = SD.open(top.c_str(), FILE_READ);
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    SpiBusLock::unlock();
    finishJob("Delete failed");
    return false;
  }
  File child = directory.openNextFile();
  const bool found = static_cast<bool>(child);
  const bool childDirectory = found && child.isDirectory();
  const String childPath = found ? top + "/" + baseName(child.name()) : String();
  if (child) child.close();
  directory.close();
  bool removed = true;
  if (!found) removed = SD.rmdir(top.c_str());
  else if (!childDirectory) removed = SD.remove(childPath.c_str());
  SpiBusLock::unlock();
  if (!removed) { finishJob("Delete failed"); return false; }
  if (found && childDirectory) {
    if (_fsDepth >= MaxFsDepth) { finishJob("Folder too deep"); return false; }
    _fsStack[_fsDepth++] = childPath;
    return true;
  }
  ++_fsJobDone;
  if (!found) {
    _fsStack[--_fsDepth] = String();
    if (_fsDepth == 0) { finishJob(nullptr); return false; }
  }
  return true;
}

void AppRuntime::serviceJob() {
  if (_fsJob == FsJob::None) return;
  if (!_card.isMounted()) { finishJob("SD card removed"); return; }
  const int64_t deadline = esp_timer_get_time() + 8000;
  while (stepJob() && !_cancel && esp_timer_get_time() < deadline) taskYIELD();
}

void AppRuntime::releaseFs() {
  closeScan(false);
  finishJob(nullptr);
  _fsLastJob = FsJob::None;
  _fsJobFinished = false;
  _fsJobDone = 0;
  _fsJobSource = _fsJobTarget = String();
  _fsScanPath = _fsScanError = String();
}

int AppRuntime::luaFsScan(lua_State *state) {
  AppRuntime *runtime = self(state);
  String path;
  if (!runtime->checkFsPath(state, 1, true, false, false, path)) return 2;
  if (runtime->_fsJob != FsJob::None) return pushFailure(state, false, "File job active");
  String error;
  const int64_t started = callbackClockUs();
  const bool success = runtime->startScan(path, error);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!success) return pushFailure(state, false, error.c_str());
  lua_pushboolean(state, true);
  return 1;
}

int AppRuntime::luaFsScanStatus(lua_State *state) {
  AppRuntime *runtime = self(state);
  lua_createtable(state, 0, 6);
  lua_pushboolean(state, runtime->_fsScanning); lua_setfield(state, -2, "scanning");
  lua_pushboolean(state, runtime->_fsScanReady); lua_setfield(state, -2, "ready");
  lua_pushinteger(state, runtime->_fsCount); lua_setfield(state, -2, "count");
  lua_pushboolean(state, runtime->_fsTruncated); lua_setfield(state, -2, "truncated");
  lua_pushstring(state, runtime->_fsScanPath.c_str()); lua_setfield(state, -2, "path");
  if (!runtime->_fsScanError.isEmpty()) {
    lua_pushstring(state, runtime->_fsScanError.c_str()); lua_setfield(state, -2, "error");
  }
  return 1;
}

int AppRuntime::luaFsEntries(lua_State *state) {
  AppRuntime *runtime = self(state);
  const lua_Integer offset = luaL_checkinteger(state, 1);
  const lua_Integer count = luaL_checkinteger(state, 2);
  if (!runtime->_fsScanReady) return pushFailure(state, true, "No listing");
  if (offset < 0 || offset > runtime->_fsCount || count < 1 || count > 16)
    return pushFailure(state, true, "Invalid range");
  const lua_Integer available = runtime->_fsCount - offset;
  const int total = static_cast<int>(count < available ? count : available);
  lua_createtable(state, total, 0);
  for (int i = 0; i < total; ++i) {
    const FsEntry &entry = runtime->_fsEntries[offset + i];
    pushFsEntry(state, runtime->_fsNames + entry.nameOffset, entry.directory,
                entry.size, entry.modified, entry.format);
    lua_rawseti(state, -2, i + 1);
  }
  return 1;
}

int AppRuntime::luaFsStat(lua_State *state) {
  AppRuntime *runtime = self(state);
  String path;
  if (!runtime->checkFsPath(state, 1, true, false, true, path)) return 2;
  const int64_t started = callbackClockUs();
  bool found = false, directory = false;
  uint32_t size = 0, modified = 0;
  const bool locked = SpiBusLock::lock(AppPackage::SdLockTimeout);
  if (locked) {
    File file = SD.open(path.c_str(), FILE_READ);
    found = static_cast<bool>(file);
    if (found) {
      directory = file.isDirectory();
      size = directory ? 0 : static_cast<uint32_t>(file.size());
      modified = static_cast<uint32_t>(file.getLastWrite());
      file.close();
    }
    SpiBusLock::unlock();
  }
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!locked) return pushFailure(state, true, "SD busy");
  if (!found) return pushFailure(state, true, "File not found");
  const String name = path == "/" ? path : baseName(path);
  pushFsEntry(state, name.c_str(), directory, size, modified,
              static_cast<uint8_t>(directory ? FileFormats::Format::Unknown
                                             : FileFormats::fromPath(name)));
  return 1;
}

int AppRuntime::luaFsMkdir(lua_State *state) {
  AppRuntime *runtime = self(state);
  String path;
  if (!runtime->checkFsPath(state, 1, false, true, false, path)) return 2;
  if (runtime->_fsJob != FsJob::None) return pushFailure(state, false, "File job active");
  const int64_t started = callbackClockUs();
  const char *error = nullptr;
  if (!SpiBusLock::lock(AppPackage::SdLockTimeout)) {
    error = "SD busy";
  } else {
    if (SD.exists(path.c_str())) error = "Already exists";
    else if (!SD.mkdir(path.c_str())) error = "Cannot create folder";
    SpiBusLock::unlock();
  }
  if (!error) runtime->closeScan(false);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (error) return pushFailure(state, false, error);
  lua_pushboolean(state, true);
  return 1;
}

int AppRuntime::luaFsRename(lua_State *state) {
  AppRuntime *runtime = self(state);
  // Raise argument type errors before any String owns heap memory.
  luaL_checkstring(state, 1);
  luaL_checkstring(state, 2);
  String source, target;
  if (!runtime->checkFsPath(state, 1, false, true, false, source)) return 2;
  if (!runtime->checkFsPath(state, 2, false, true, false, target)) return 2;
  if (FileFormats::isInternal(FileFormats::fromPath(target)))
    return pushFailure(state, false, "Invalid path");
  if (source.equalsIgnoreCase(target)) return pushFailure(state, false, "Target exists");
  if (PathPolicy::atOrBelow(target, source))
    return pushFailure(state, false, "Cannot move a folder into itself");
  String error;
  const int64_t started = callbackClockUs();
  const bool success = runtime->startJob(FsJob::Rename, source, target, error);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!success) return pushFailure(state, false, error.c_str());
  lua_pushboolean(state, true);
  return 1;
}

int AppRuntime::luaFsRemove(lua_State *state) {
  AppRuntime *runtime = self(state);
  String path;
  if (!runtime->checkFsPath(state, 1, false, true, false, path)) return 2;
  String error;
  const int64_t started = callbackClockUs();
  const bool success = runtime->startJob(FsJob::Remove, path, String(), error);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!success) return pushFailure(state, false, error.c_str());
  lua_pushboolean(state, true);
  return 1;
}

int AppRuntime::luaFsJob(lua_State *state) {
  AppRuntime *runtime = self(state);
  lua_createtable(state, 0, 5);
  lua_pushboolean(state, runtime->_fsJob != FsJob::None); lua_setfield(state, -2, "active");
  if (runtime->_fsLastJob != FsJob::None) {
    lua_pushstring(state, runtime->_fsLastJob == FsJob::Rename ? "rename" : "remove");
    lua_setfield(state, -2, "kind");
  }
  lua_pushinteger(state, runtime->_fsJobDone); lua_setfield(state, -2, "done");
  lua_pushboolean(state, runtime->_fsJobFinished); lua_setfield(state, -2, "finished");
  if (!runtime->_fsJobError.isEmpty()) {
    lua_pushstring(state, runtime->_fsJobError.c_str()); lua_setfield(state, -2, "error");
  }
  return 1;
}

int AppRuntime::luaFsCancel(lua_State *state) {
  AppRuntime *runtime = self(state);
  const int64_t started = callbackClockUs();
  if (runtime->_fsScanning) runtime->closeScan(false);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (runtime->_fsJob != FsJob::None) runtime->_fsJobCancel = true;
  return 0;
}

int AppRuntime::luaFsOpen(lua_State *state) {
  AppRuntime *runtime = self(state);
  String path;
  if (!runtime->checkFsPath(state, 1, false, false, false, path)) return 2;
  if (runtime->_fsJob != FsJob::None) return pushFailure(state, false, "File job active");
  if (runtime->_navigationPending) return pushFailure(state, false, "Navigation pending");
  const FileFormats::Format format = FileFormats::fromPath(path);
  if (!FileFormats::isOpenable(format)) return pushFailure(state, false, "Unsupported file");
  const int64_t started = callbackClockUs();
  bool found = false;
  const bool locked = SpiBusLock::lock(AppPackage::SdLockTimeout);
  if (locked) {
    File file = SD.open(path.c_str(), FILE_READ);
    found = file && !file.isDirectory();
    if (file) file.close();
    SpiBusLock::unlock();
  }
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!locked) return pushFailure(state, false, "SD busy");
  if (!found) return pushFailure(state, false, "File not found");
  return runtime->requestOpenFile(state, path);
}

int AppRuntime::requestOpenFile(lua_State *state, const String &path) {
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return pushFailure(state, false, "Host busy");
  _navigation = Navigation{};
  _navigation.kind = NavigationKind::OpenFile;
  copyText(_navigation.path, sizeof(_navigation.path), path.c_str());
  _navigationPending = true;
  xSemaphoreGive(_mutex);
  lua_pushboolean(state, true);
  return 1;
}

int AppRuntime::luaImageOpen(lua_State *state) {
  AppRuntime *runtime = self(state);
  String path;
  if (!runtime->checkFsPath(state, 1, false, false, false, path)) return 2;
  if (runtime->_fsJob != FsJob::None) return pushFailure(state, false, "File job active");
  const FileFormats::Format format = FileFormats::fromPath(path);
  if (!FileFormats::isImage(format) || !FileFormats::isOpenable(format))
    return pushFailure(state, false, "Unsupported file");
  const int64_t started = callbackClockUs();
  bool found = false;
  const bool locked = SpiBusLock::lock(AppPackage::SdLockTimeout);
  if (locked) {
    File file = SD.open(path.c_str(), FILE_READ);
    found = file && !file.isDirectory();
    if (file) file.close();
    SpiBusLock::unlock();
  }
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!locked) return pushFailure(state, false, "SD busy");
  if (!found) return pushFailure(state, false, "File not found");
  if (xSemaphoreTake(runtime->_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return pushFailure(state, false, "Host busy");
  runtime->_image.shown = true;
  runtime->_image.zoom = false;
  ++runtime->_image.serial;
  copyText(runtime->_image.path, sizeof(runtime->_image.path), path.c_str());
  ++runtime->_imageRevision;
  xSemaphoreGive(runtime->_mutex);
  lua_pushboolean(state, true);
  return 1;
}

int AppRuntime::luaImageStatus(lua_State *state) {
  AppRuntime *runtime = self(state);
  ImageLayer layer{};
  ImageStatus status{};
  if (xSemaphoreTake(runtime->_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    layer = runtime->_image;
    status = runtime->_imageStatus;
    xSemaphoreGive(runtime->_mutex);
  }
  // Until the host reports on the latest open, that image is still loading.
  if (!layer.shown) status = ImageStatus{};
  else if (status.serial != layer.serial || status.state == ImageState::None) {
    status = ImageStatus{};
    status.state = ImageState::Loading;
  }
  const char *name = "none";
  if (status.state == ImageState::Loading) name = "loading";
  else if (status.state == ImageState::Ready) name = "ready";
  else if (status.state == ImageState::Error) name = "error";
  lua_newtable(state);
  lua_pushstring(state, name); lua_setfield(state, -2, "state");
  lua_pushstring(state, layer.shown ? layer.path : ""); lua_setfield(state, -2, "path");
  lua_pushinteger(state, status.width); lua_setfield(state, -2, "width");
  lua_pushinteger(state, status.height); lua_setfield(state, -2, "height");
  lua_pushstring(state, status.format); lua_setfield(state, -2, "format");
  lua_pushboolean(state, status.animated); lua_setfield(state, -2, "animated");
  lua_pushboolean(state, status.zoom); lua_setfield(state, -2, "zoom");
  lua_pushboolean(state, status.zoomAvailable); lua_setfield(state, -2, "zoom_available");
  lua_pushboolean(state, layer.barVisible); lua_setfield(state, -2, "bar");
  if (status.state == ImageState::Error) {
    lua_pushstring(state, status.error); lua_setfield(state, -2, "error");
  }
  return 1;
}

int AppRuntime::luaImageSetZoom(lua_State *state) {
  AppRuntime *runtime = self(state);
  luaL_checktype(state, 1, LUA_TBOOLEAN);
  const bool zoom = lua_toboolean(state, 1);
  if (xSemaphoreTake(runtime->_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return pushFailure(state, false, "Host busy");
  const char *error = nullptr;
  if (!runtime->_image.shown) error = "No image";
  else if (runtime->_imageStatus.serial != runtime->_image.serial ||
           !runtime->_imageStatus.zoomAvailable) error = "Zoom unavailable";
  if (!error && runtime->_image.zoom != zoom) {
    runtime->_image.zoom = zoom;
    ++runtime->_imageRevision;
  }
  xSemaphoreGive(runtime->_mutex);
  if (error) return pushFailure(state, false, error);
  lua_pushboolean(state, true);
  return 1;
}

int AppRuntime::luaImageSetBar(lua_State *state) {
  AppRuntime *runtime = self(state);
  luaL_checktype(state, 1, LUA_TBOOLEAN);
  const bool visible = lua_toboolean(state, 1);
  if (xSemaphoreTake(runtime->_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return pushFailure(state, false, "Host busy");
  if (runtime->_image.barVisible != visible) {
    runtime->_image.barVisible = visible;
    ++runtime->_imageRevision;
  }
  xSemaphoreGive(runtime->_mutex);
  lua_pushboolean(state, true);
  return 1;
}

int AppRuntime::luaImageClose(lua_State *state) {
  self(state)->closeImage();
  return 0;
}

void AppRuntime::closeImage() {
  if (!_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return;
  if (_image.shown || !_image.barVisible) {
    _image.shown = false;
    _image.zoom = false;
    _image.barVisible = true;
    ++_imageRevision;
  }
  xSemaphoreGive(_mutex);
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
  if (_sdkVersion >= 3) {
    lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaStorageSize, 1);
    lua_setfield(_lua, -2, "size");
  }
  lua_setfield(_lua, -2, "storage");
  lua_newtable(_lua);
  lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaStatus, 1); lua_setfield(_lua, -2, "status");
  lua_setfield(_lua, -2, "device");
  lua_newtable(_lua);
  lua_pushcfunction(_lua, luaMillis); lua_setfield(_lua, -2, "millis");
  lua_setfield(_lua, -2, "time");
  if (_sdkVersion >= 2) {
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
    if (_sdkVersion >= 3) {
      static const luaL_Reg fileFunctions[] = {
        {"play_file", luaPlayFile}, {"stop_file", luaStopFile},
        {"playback", luaPlaybackStatus}, {nullptr, nullptr}};
      for (const luaL_Reg *entry = fileFunctions; entry->name; ++entry) {
        lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, entry->func, 1);
        lua_setfield(_lua, -2, entry->name);
      }
    }
    lua_setfield(_lua, -2, "audio");
    if (_sdkVersion >= 3) {
      static const luaL_Reg micFunctions[] = {
        {"status", luaMicStatus}, {"start", luaMicStart}, {"stop", luaMicStop},
        {nullptr, nullptr}};
      lua_newtable(_lua);
      for (const luaL_Reg *entry = micFunctions; entry->name; ++entry) {
        lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, entry->func, 1);
        lua_setfield(_lua, -2, entry->name);
      }
      lua_setfield(_lua, -2, "mic");
    }
  }
  if (_sdkVersion >= 4) {
    lua_getfield(_lua, -1, "ui");
    lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaListCell, 1);
    lua_setfield(_lua, -2, "list_cell");
    lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, luaPrompt, 1);
    lua_setfield(_lua, -2, "prompt");
    lua_pop(_lua, 1);
    if (_filesPermission) {
      static const luaL_Reg fsFunctions[] = {
        {"scan", luaFsScan}, {"scan_status", luaFsScanStatus}, {"entries", luaFsEntries},
        {"stat", luaFsStat}, {"mkdir", luaFsMkdir}, {"rename", luaFsRename},
        {"remove", luaFsRemove}, {"job", luaFsJob}, {"cancel", luaFsCancel},
        {"open", luaFsOpen}, {nullptr, nullptr}};
      lua_newtable(_lua);
      for (const luaL_Reg *entry = fsFunctions; entry->name; ++entry) {
        lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, entry->func, 1);
        lua_setfield(_lua, -2, entry->name);
      }
      lua_setfield(_lua, -2, "fs");
      if (_sdkVersion >= 5) {
        static const luaL_Reg imageFunctions[] = {
          {"open", luaImageOpen}, {"status", luaImageStatus}, {"set_zoom", luaImageSetZoom},
          {"set_bar", luaImageSetBar}, {"close", luaImageClose}, {nullptr, nullptr}};
        lua_newtable(_lua);
        for (const luaL_Reg *entry = imageFunctions; entry->name; ++entry) {
          lua_pushlightuserdata(_lua, this); lua_pushcclosure(_lua, entry->func, 1);
          lua_setfield(_lua, -2, entry->name);
        }
        lua_setfield(_lua, -2, "image");
      }
    }
  }
  if (_sdkVersion >= 6) registerSdk6();
  lua_setglobal(_lua, "esp");
}

void AppRuntime::setError(const String &message) {
  Serial.printf("[SD app] %s: %s\n", _id.c_str(), message.c_str());
  Scene scene{};
  copyText(scene.title, sizeof(scene.title), "App error");
  scene.count = 3;
  copyText(scene.rows[0].text, sizeof(scene.rows[0].text), message.c_str());
  // The host handles these once the worker has stopped.
  scene.rows[1].button = true;
  copyText(scene.rows[1].id, sizeof(scene.rows[1].id), HostRestartAction);
  copyText(scene.rows[1].text, sizeof(scene.rows[1].text), "Restart app");
  scene.rows[2].button = true;
  copyText(scene.rows[2].id, sizeof(scene.rows[2].id), HostCloseAction);
  copyText(scene.rows[2].text, sizeof(scene.rows[2].text), "Close app");
  if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    _scene = scene;
    ++_revision;
    xSemaphoreGive(_mutex);
  }
}

bool AppRuntime::invoke(const char *callback) { return invoke(callback, CallArgs()); }

bool AppRuntime::invoke(const char *callback, const CallArgs &callArguments) {
  lua_rawgeti(_lua, LUA_REGISTRYINDEX, _appRef);
  lua_getfield(_lua, -1, callback);
  if (lua_isnil(_lua, -1)) {
    lua_settop(_lua, 0);
    if (strcmp(callback, "on_start") == 0) { setError("Missing on_start callback"); return false; }
    return true;
  }
  if (!lua_isfunction(_lua, -1)) { lua_settop(_lua, 0); setError("Invalid app callback"); return false; }
  int arguments = 0;
  if (callArguments.text) { lua_pushstring(_lua, callArguments.text); ++arguments; }
  else if (callArguments.nil) { lua_pushnil(_lua); ++arguments; }
  for (uint8_t i = 0; i < callArguments.numbers; ++i) {
    lua_pushinteger(_lua, static_cast<lua_Integer>(callArguments.number[i]));
    ++arguments;
  }
  _callbackName = callback;
  return callPrepared(arguments);
}

bool AppRuntime::callPrepared(int arguments) {
  payGcDebt();
  _hookCount = 0;
  const int64_t wallStartedUs = esp_timer_get_time();
  _callbackStartedUs = callbackClockUs();
  lua_sethook(_lua, luaHook, LUA_MASKCOUNT, 1000);
  const int result = lua_pcall(_lua, arguments, 0, 0);
  lua_sethook(_lua, nullptr, 0, 0);
  if (result != LUA_OK || callbackClockUs() - _callbackStartedUs > CallbackMaxUs) {
    const char *error = lua_tostring(_lua, -1);
    Serial.printf("[SD app] %s: %s ended after %lld ms of script CPU (%lld ms wall), %u instructions\n",
                  _id.c_str(), _callbackName ? _callbackName : "callback",
                  (callbackClockUs() - _callbackStartedUs) / 1000,
                  (esp_timer_get_time() - wallStartedUs) / 1000,
                  static_cast<unsigned>(_hookCount));
    setError(result != LUA_OK && error ? error : "App callback exceeded its time limit");
    lua_settop(_lua, 0);
    return false;
  }
  lua_settop(_lua, 0);
  return true;
}

// The clock for the callback limit: CPU time the calling task (the app
// worker) has used, in microseconds. Wall time would also count every slice
// in which Wi-Fi, TLS, or another worker preempted the script. FreeRTOS
// updates a task's run-time counter only at context switches, so yield first
// to bring the running task's figure up to date; at the worker's priority
// that returns at once unless an equal-priority task is ready.
int64_t AppRuntime::callbackClockUs() {
  static uint32_t last = 0;
  static int64_t wraps = 0;
  taskYIELD();
  TaskStatus_t status{};
  vTaskGetInfo(nullptr, &status, pdFALSE, eRunning);
  const uint32_t now = static_cast<uint32_t>(status.ulRunTimeCounter);
  if (now < last) wraps += int64_t(1) << 32;
  last = now;
  return wraps + now;
}

// Lua 5.4's incremental collector pays its whole outstanding debt at the next
// allocation. Host work that allocates Lua objects outside a callback (the
// compiler, response tables) would otherwise make the next timed callback pay
// for it, which on a PSRAM heap can exceed the time limit. Paying the debt
// here keeps that work outside the timer.
void AppRuntime::payGcDebt() {
  lua_gc(_lua, LUA_GCSTEP, 1);
}

void AppRuntime::taskMain(void *context) {
  AppRuntime *runtime = static_cast<AppRuntime *>(context);
  runtime->run();
  runtime->_stopped = true;
  vTaskDelay(portMAX_DELAY);
}

void AppRuntime::run() {
  Serial.printf("[SD app] %s: starting (sd=%s, heap=%u, psram=%u)\n", _id.c_str(),
                _card.isMounted() ? "mounted" : "unmounted",
                static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getFreePsram()));
  AppPackage package = AppPackage::load(_card, _id);
  if (!package.valid) { setError(package.error); return; }
  _displayName = package.name;
  _sdkVersion = package.sdk;
  _filesPermission = package.sdk >= 4 && package.filesPermission;
  _networkPermission = package.sdk >= 6 && package.networkPermission && package.hostCount > 0;
  _hostCount = _networkPermission ? package.hostCount : 0;
  for (uint8_t i = 0; i < _hostCount; ++i) _hosts[i] = package.hosts[i];
  if (_networkPermission) {
    String folder = String("/Downloads/") + _displayName;
    String checked;
    if (!PathPolicy::appPath(folder.c_str(), folder.length(), false, checked) ||
        _displayName.indexOf('/') >= 0)
      folder = String("/Downloads/") + _id;
    _downloads = folder;
    _net = new (std::nothrow) AppNetWorker();
    if (!_net) { setError("App memory unavailable"); return; }
    _net->configure(_hosts, _hostCount,
                    String("ESP-Media-App/") + FirmwareVersion::Text + " (" + _id + ")",
                    _card, _downloads);
  }
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
    payGcDebt();
    _hookCount = 0;
    _callbackStartedUs = callbackClockUs();
    lua_sethook(_lua, luaHook, LUA_MASKCOUNT, 1000);
    const int result = lua_pcall(_lua, 0, 1, 0);
    lua_sethook(_lua, nullptr, 0, 0);
    if (result != LUA_OK || callbackClockUs() - _callbackStartedUs > CallbackMaxUs ||
        !lua_istable(_lua, -1)) {
      if (result != LUA_OK) {
        const char *startupError = lua_tostring(_lua, -1);
        setError(startupError ? startupError : "App startup failed");
      }
      else if (callbackClockUs() - _callbackStartedUs > CallbackMaxUs)
        setError("App startup exceeded its time limit");
      else setError("App must return a callback table");
    } else {
      _appRef = luaL_ref(_lua, LUA_REGISTRYINDEX);
      if (invoke("on_start")) {
        uint32_t lastTick = millis();
        while (!_cancel && _card.isMounted()) {
          if (_paused) {
            if (!_resumeRequested) { vTaskDelay(pdMS_TO_TICKS(20)); continue; }
            if (!resumeFromNavigation()) break;
            lastTick = millis();
            continue;
          }
          if (_navigationPending) { pauseForNavigation(); continue; }
          Action action{};
          const bool fsBusy = _fsScanning || _fsJob != FsJob::None;
          const uint32_t waitMs = fsBusy ? 1 :
                                  (_toneRemaining || _toneCount || _audioReady ||
                                   _recActive || _playActive ||
                                   (_net && _net->active())) ? 10 : TickIntervalMs;
          if (xQueueReceive(_actions, &action, pdMS_TO_TICKS(waitMs)) == pdTRUE) {
            CallArgs arguments;
            const char *callback = "on_action";
            if (action.kind == ActionKind::Button) {
              arguments.text = action.id;
            } else {
              arguments.number[0] = action.x;
              arguments.number[1] = action.y;
              arguments.numbers = 2;
              callback = "on_touch";
              if (action.kind == ActionKind::TouchEnd) {
                arguments.number[2] = action.heldMs;
                arguments.numbers = 3;
                callback = "on_touch_end";
              }
            }
            if (action.kind != ActionKind::TouchEnd || _sdkVersion >= 4) {
              if (!invoke(callback, arguments)) break;
            }
          }
          serviceTones();
          serviceRecording();
          servicePlayback();
          serviceScan();
          serviceJob();
          if (_net && !serviceNet()) break;
          const uint32_t now = millis();
          if (now - lastTick >= TickIntervalMs) {
            lastTick = now;
            CallArgs arguments;
            arguments.number[0] = now;
            arguments.numbers = 1;
            if (!invoke("on_tick", arguments)) break;
          }
        }
      }
    }
  }
  releaseFs();
  finishRecording("");
  finishPlayback(nullptr, false);
  clearTones();
  if (_net) _net->cancel(0);
  lua_close(_lua);
  _lua = nullptr;
  _appRef = LUA_NOREF;
}
