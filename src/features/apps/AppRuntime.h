#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class SdCardController;
struct lua_State;
struct lua_Debug;

class AppRuntime {
public:
  static constexpr uint8_t MaxRows = 10;
  struct Row {
    bool button;
    char id[33];
    char text[97];
  };
  struct Scene {
    char title[41];
    uint8_t count;
    Row rows[MaxRows];
  };
  struct Status {
    uint16_t batteryVoltageMv;
    bool wifiConnected;
    bool sdMounted;
    int64_t epochSeconds;
  };

  explicit AppRuntime(SdCardController &card);
  bool start(const String &id, const Status &initialStatus);
  void requestStop();
  bool stopped() const;
  bool end();
  bool sendAction(const char *id);
  bool takeScene(Scene &scene, uint32_t &revision);
  void publishStatus(const Status &status);

private:
  static constexpr size_t LuaMemoryLimit = 512 * 1024;
  static constexpr uint32_t CallbackMaxInstructions = 100000;
  static constexpr int64_t CallbackMaxUs = 20000;
  static constexpr uint32_t TickIntervalMs = 100;
  static constexpr uint32_t WorkerStackBytes = 16384;
  struct Action { char id[33]; };

  SdCardController &_card;
  String _id;
  String _displayName;
  TaskHandle_t _task = nullptr;
  QueueHandle_t _actions = nullptr;
  SemaphoreHandle_t _mutex = nullptr;
  volatile bool _cancel = false;
  volatile bool _stopped = true;
  Scene _scene{};
  Status _status{};
  uint32_t _revision = 0;
  size_t _luaBytes = 0;
  uint32_t _hookCount = 0;
  int64_t _callbackStartedUs = 0;
  lua_State *_lua = nullptr;
  int _appRef = -1;

  static void taskMain(void *context);
  void run();
  void setError(const String &message);
  bool invoke(const char *callback, const char *action = nullptr, uint32_t nowMs = 0);
  void registerApi();
  static void *luaAlloc(void *context, void *pointer, size_t oldSize, size_t newSize);
  static void luaHook(lua_State *state, lua_Debug *debug);
  static AppRuntime *self(lua_State *state);
  static int luaShow(lua_State *state);
  static int luaRead(lua_State *state);
  static int luaWrite(lua_State *state);
  static int luaStatus(lua_State *state);
  static int luaMillis(lua_State *state);
  bool readData(const char *name, char *&data, size_t &length, String &error);
  bool writeData(const char *name, const char *data, size_t length, String &error);
};
