#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

class SdCardController;
class SpeakerController;
class AudioSettings;
class MicController;
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

  static constexpr uint8_t MaxDrawCommands = 64;
  enum class DrawOp : uint8_t {
    FillScreen, FillRect, DrawRect, DrawLine, HLine, VLine,
    DrawCircle, FillCircle, DrawRoundRect, FillRoundRect, FillTriangle,
    DrawWideLine, DrawSmoothCircle, FillSmoothCircle,
    DrawSmoothRoundRect, FillSmoothRoundRect, DrawSmoothArc,
    Text, Button, Progress
  };
  struct DrawCommand {
    DrawOp op;
    int32_t arg[9];
    char text[97];
  };
  struct Frame {
    char title[41];
    uint16_t background;
    uint8_t count;
    DrawCommand commands[MaxDrawCommands];
  };

  AppRuntime(SdCardController &card, SpeakerController &speaker, AudioSettings &audioSettings,
             MicController &mic);
  bool start(const String &id, const Status &initialStatus);
  void requestStop();
  bool stopped() const;
  bool end();
  bool sendAction(const char *id);
  bool sendTouch(uint16_t x, uint16_t y);
  bool takeScene(Scene &scene, uint32_t &revision);
  bool takeFrame(Frame &frame, uint32_t &revision);
  void publishStatus(const Status &status);

private:
  static constexpr size_t LuaMemoryLimit = 512 * 1024;
  static constexpr uint32_t CallbackMaxInstructions = 100000;
  static constexpr int64_t CallbackMaxUs = 20000;
  static constexpr uint32_t TickIntervalMs = 100;
  static constexpr uint32_t WorkerStackBytes = 16384;
  struct Action { char id[33]; uint16_t x; uint16_t y; bool touch; };
  struct Tone { uint16_t frequency; uint16_t durationMs; };

  SdCardController &_card;
  SpeakerController &_speaker;
  AudioSettings &_audioSettings;
  MicController &_mic;
  String _id;
  String _displayName;
  TaskHandle_t _task = nullptr;
  QueueHandle_t _actions = nullptr;
  SemaphoreHandle_t _mutex = nullptr;
  volatile bool _cancel = false;
  volatile bool _stopped = true;
  Scene _scene{};
  Frame *_buildingFrame = nullptr;
  Frame *_publishedFrame = nullptr;
  uint32_t _frameRevision = 0;
  bool _frameOpen = false;
  uint32_t _frameCost = 0;
  uint8_t _sdkVersion = 1;
  Tone _tones[8]{};
  uint8_t _toneHead = 0;
  uint8_t _toneCount = 0;
  uint32_t _toneRemaining = 0;
  uint32_t _tonePhase = 0;
  uint32_t _toneStep = 0;
  uint32_t _toneLastWriteMs = 0;
  bool _audioReady = false;
  // Also the WAV playback staging buffer; tones and file playback never overlap.
  int16_t _toneBuffer[256]{};
  // Microphone recording (SDK 3). Only the worker task touches this state.
  bool _recActive = false;
  bool _recToFile = false;
  fs::File _recFile;
  String _recPath;
  String _recTempPath;
  String _recError;
  uint32_t _recBytes = 0;
  uint32_t _recMaxBytes = 0;
  uint32_t _recFlushedBytes = 0;
  uint32_t _recLevelMs = 0;
  uint8_t _recLevel = 0;
  uint16_t _recPending = 0;
  int16_t _recBuffer[512]{};
  // WAV file playback (SDK 3).
  bool _playActive = false;
  bool _playFinished = false;
  fs::File _playFile;
  String _playError;
  uint32_t _playSampleRate = 16000;
  uint32_t _playRemainingBytes = 0;
  uint32_t _playWrittenBytes = 0;
  uint16_t _playPendingSamples = 0;
  uint16_t _playPendingOffset = 0;
  uint32_t _playLastWriteMs = 0;
  uint32_t _playEndedMs = 0;
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
  bool invoke(const char *callback, const char *action = nullptr,
              uint32_t nowMs = 0, uint32_t touchY = 0);
  void registerApi();
  static void *luaAlloc(void *context, void *pointer, size_t oldSize, size_t newSize);
  static void luaHook(lua_State *state, lua_Debug *debug);
  static AppRuntime *self(lua_State *state);
  static int luaShow(lua_State *state);
  static int luaRead(lua_State *state);
  static int luaWrite(lua_State *state);
  static int luaStatus(lua_State *state);
  static int luaMillis(lua_State *state);
  static int luaBeginFrame(lua_State *state);
  static int luaPresent(lua_State *state);
  static int luaDraw(lua_State *state);
  static int luaPlayTone(lua_State *state);
  static int luaStopTones(lua_State *state);
  static int luaVolume(lua_State *state);
  static int luaSetVolume(lua_State *state);
  static int luaStorageSize(lua_State *state);
  static int luaMicStatus(lua_State *state);
  static int luaMicStart(lua_State *state);
  static int luaMicStop(lua_State *state);
  static int luaPlayFile(lua_State *state);
  static int luaStopFile(lua_State *state);
  static int luaPlaybackStatus(lua_State *state);
  void clearTones();
  void serviceTones();
  bool ensureDataDirectory();  // Caller holds SpiBusLock.
  bool startRecording(const char *name, uint32_t maxSeconds, String &error);
  // Finalizes any open recording; `reason` is empty for an app-requested stop.
  bool finishRecording(const char *reason, uint32_t *bytes = nullptr);
  void serviceRecording();
  bool startPlayback(const char *name, String &error);
  void finishPlayback(const char *error, bool finished);
  void servicePlayback();
  bool readData(const char *name, char *&data, size_t &length, String &error);
  bool writeData(const char *name, const char *data, size_t length, String &error);
};
