#pragma once

#include <Arduino.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "../../media/MediaCache.h"

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
    Text, Button, Progress, ListCell
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

  // A host-screen navigation an SDK 4 app asked for. The worker never
  // navigates; the host screen takes the request once the app is paused.
  enum class NavigationKind : uint8_t { None, OpenFile, Prompt };
  struct Navigation {
    NavigationKind kind;
    char path[256];     // OpenFile: validated absolute SD path.
    char title[41];     // Prompt: keyboard title.
    char initial[256];  // Prompt: initial text.
  };

  // The host image layer an SDK 5 app asked for. The worker only records what
  // it wants; the host screen opens, zooms, and closes the shared `ImageView`
  // on the UI task and reports back through `publishImage`.
  struct ImageLayer {
    bool shown;
    bool zoom;
    bool barVisible;
    uint32_t serial;  // Changes with every `esp.image.open`.
    char path[256];
  };
  enum class ImageState : uint8_t { None, Loading, Ready, Error };
  struct ImageStatus {
    ImageState state;
    uint32_t serial;  // The `ImageLayer::serial` this status describes.
    uint32_t width;
    uint32_t height;
    bool animated;
    bool zoom;
    bool zoomAvailable;
    char format[8];
    char error[64];
  };

  AppRuntime(SdCardController &card, SpeakerController &speaker, AudioSettings &audioSettings,
             MicController &mic, MediaCache &mediaCache);
  bool start(const String &id, const Status &initialStatus);
  void requestStop();
  bool stopped() const;
  bool end();
  bool sendAction(const char *id);
  // Canvas coordinates; `y` is negative above the canvas while an image
  // layer hides the app bar.
  bool sendTouch(int16_t x, int16_t y);
  bool sendTouchEnd(int16_t x, int16_t y, uint32_t heldMs);
  // True once a navigation is requested and the worker has released audio and
  // stopped callbacks. The app stays paused until resume().
  bool takeNavigation(Navigation &navigation);
  // Continues a paused app. `input` is the keyboard result for a prompt, or
  // null when it was cancelled; it is ignored after OpenFile.
  void resume(const char *input);
  bool takeScene(Scene &scene, uint32_t &revision);
  bool takeFrame(Frame &frame, uint32_t &revision);
  void publishStatus(const Status &status);
  bool takeImage(ImageLayer &layer, uint32_t &revision);
  void publishImage(const ImageStatus &status);

private:
  static constexpr size_t LuaMemoryLimit = 512 * 1024;
  static constexpr uint32_t CallbackMaxInstructions = 100000;
  static constexpr int64_t CallbackMaxUs = 20000;
  static constexpr uint32_t TickIntervalMs = 100;
  static constexpr uint32_t WorkerStackBytes = 16384;
  static constexpr uint16_t MaxFsEntries = 2000;
  static constexpr uint8_t MaxFsDepth = 16;
  enum class ActionKind : uint8_t { Button, Touch, TouchEnd };
  struct Action { char id[33]; int16_t x; int16_t y; uint32_t heldMs; ActionKind kind; };
  struct CallArgs {
    const char *text = nullptr;
    bool nil = false;  // Push nil as the first argument instead of text.
    int64_t number[3] = {0, 0, 0};
    uint8_t numbers = 0;
  };
  struct FsEntry {
    uint32_t nameOffset;
    uint32_t size;
    uint32_t modified;
    uint8_t format;
    bool directory;
  };
  enum class FsJob : uint8_t { None, Rename, Remove };
  enum class FsPhase : uint8_t { SourceCleanup, TargetCleanup, Commit, Tree };
  struct Tone { uint16_t frequency; uint16_t durationMs; };

  SdCardController &_card;
  SpeakerController &_speaker;
  AudioSettings &_audioSettings;
  MicController &_mic;
  MediaCache &_mediaCache;
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
  bool _filesPermission = false;
  // Navigation hand-off (SDK 4). `_navigation`, `_input`, and their flags are
  // guarded by `_mutex`; `_paused` is written only by the worker.
  Navigation _navigation{};
  char _input[256]{};
  bool _navigationPending = false;
  bool _inputProvided = false;
  NavigationKind _pausedFor = NavigationKind::None;
  volatile bool _paused = false;
  volatile bool _resumeRequested = false;
  // Host image layer (SDK 5 `esp.image`), guarded by `_mutex`.
  ImageLayer _image{};
  ImageStatus _imageStatus{};
  uint32_t _imageRevision = 0;
  // General SD access (SDK 4 `esp.fs`). Only the worker task touches this.
  FsEntry *_fsEntries = nullptr;
  char *_fsNames = nullptr;
  uint32_t _fsNamesUsed = 0;
  uint32_t _fsNamesCapacity = 0;
  uint16_t _fsCount = 0;
  uint16_t _fsCapacity = 0;
  fs::File _fsDirectory;
  String _fsScanPath;
  String _fsScanError;
  bool _fsScanning = false;
  bool _fsScanReady = false;
  bool _fsTruncated = false;
  FsJob _fsJob = FsJob::None;
  FsJob _fsLastJob = FsJob::None;
  FsPhase _fsPhase = FsPhase::Commit;
  String _fsJobSource;
  String _fsJobTarget;
  String _fsJobError;
  String _fsStack[MaxFsDepth];
  uint8_t _fsDepth = 0;
  uint32_t _fsJobDone = 0;
  bool _fsJobFinished = false;
  bool _fsJobCancel = false;
  MediaCache::Cleanup _fsCleanup;
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
  bool invoke(const char *callback);
  bool invoke(const char *callback, const CallArgs &arguments);
  void pauseForNavigation();
  bool resumeFromNavigation();
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
  static int luaListCell(lua_State *state);
  static int luaPrompt(lua_State *state);
  static int luaFsScan(lua_State *state);
  static int luaFsScanStatus(lua_State *state);
  static int luaFsEntries(lua_State *state);
  static int luaFsStat(lua_State *state);
  static int luaFsMkdir(lua_State *state);
  static int luaFsRename(lua_State *state);
  static int luaFsRemove(lua_State *state);
  static int luaFsJob(lua_State *state);
  static int luaFsCancel(lua_State *state);
  static int luaFsOpen(lua_State *state);
  static int luaImageOpen(lua_State *state);
  static int luaImageStatus(lua_State *state);
  static int luaImageSetZoom(lua_State *state);
  static int luaImageSetBar(lua_State *state);
  static int luaImageClose(lua_State *state);
  void closeImage();
  // Reads Lua argument `index` as an app path; on failure pushes the
  // `false|nil, error` pair and returns false.
  bool checkFsPath(lua_State *state, int index, bool allowRoot, bool mutation,
                   bool nilOnError, String &path);
  static void pushFsEntry(lua_State *state, const char *name, bool directory,
                          uint32_t size, uint32_t modified, uint8_t format);
  bool startScan(const String &path, String &error);
  void closeScan(bool keepListing);
  void serviceScan();
  bool appendScanEntry(const String &name, bool directory, uint32_t size, uint32_t modified);
  bool startJob(FsJob job, const String &source, const String &target, String &error);
  void finishJob(const char *error);
  void serviceJob();
  bool stepJob();
  void releaseFs();
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
