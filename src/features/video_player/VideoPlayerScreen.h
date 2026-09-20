#pragma once

#include <Arduino.h>

#include "../../core/Screen.h"
#include "../../services/audio/SpeakerController.h"
#include "../../services/storage/SdCardController.h"
#include "../../services/storage/SecretStorage.h"
#include "../../ui/UiKit.h"

class VideoStreamWorker;

// The screen owns presentation only. VideoStreamWorker owns source I/O,
// demuxing, decoding, and I2S calls on the opposite core.
class VideoPlayerScreen : public Screen {
public:
  VideoPlayerScreen(Navigator &navigator, TouchController &touch, DisplayController &display,
                    SdCardController &sdCard, SpeakerController &speaker,
                    SecretStorage &secretStorage);
  ~VideoPlayerScreen() override;

  void onEnter(const ScreenData &data) override;
  void onExit() override;
  bool releaseResources() override;
  void onResume() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  enum class State : uint8_t { Opening, Buffering, Playing, Paused, Finished, Closing, Error };

  static constexpr uint16_t ViewportTop = UiKit::AppBarHeight + 32;
  static constexpr uint16_t ViewportBottomReserve = 32;

  SdCardController &_sdCard;
  SpeakerController &_speaker;
  SecretStorage &_secretStorage;
  VideoStreamWorker *_stream;
  String _path;
  String _fileName;
  String _error;
  String _status;
  State _state;
  bool _secretAccess;
  bool _sdMounted;
  bool _lastTouchDown;
  bool _closeRequested;
  bool _autoPlay;
  uint32_t _durationMs;
  uint32_t _elapsedMs;
  uint32_t _lastDrawMs;
  uint16_t _viewportWidth;
  uint16_t _viewportHeight;

  void resetView();
  void requestClose();
  void updateStream(uint32_t nowMs);
  void presentFrames();
  void handleSdCardEvent(SdCardEvent event);
  void draw(bool full = true);
  void drawTopBar();
  void drawPlayButton();
  void drawProgress();
  void drawStatus();
  void drawError();
  bool controlContains(int16_t x, int16_t y) const;
  String displayName(const String &path) const;
};
