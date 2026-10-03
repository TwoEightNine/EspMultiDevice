#pragma once

#include <Arduino.h>

#include "../../core/DisplayOrientation.h"
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
  enum class ViewMode : uint8_t { FullScreen, Embedded };
  enum class Orientation : uint8_t { Portrait, Landscape };

  static constexpr uint16_t EmbeddedViewportTop = UiKit::AppBarHeight;
  static constexpr uint16_t EmbeddedProgressReserve = UiKit::AppBarHeight;
  static constexpr uint16_t PlaybackControlSize = 40;
  static constexpr uint16_t PlaybackControlUnderlaySize = PlaybackControlSize + 4;
  static constexpr uint32_t AutoFullScreenDelayMs = 2000;
  SdCardController &_sdCard;
  SpeakerController &_speaker;
  SecretStorage &_secretStorage;
  VideoStreamWorker *_stream;
  String _path;
  String _error;
  State _state;
  ViewMode _viewMode;
  ViewMode _pendingViewMode;
  Orientation _orientation;
  bool _secretAccess;
  bool _sdMounted;
  bool _lastTouchDown;
  bool _closeRequested;
  bool _autoPlay;
  bool _viewModeChangePending;
  bool _playButtonUnderlayValid;
  uint32_t _durationMs;
  uint32_t _elapsedMs;
  uint32_t _lastDrawMs;
  uint32_t _autoFullScreenAtMs;
  uint16_t _viewportWidth;
  uint16_t _viewportHeight;
  uint16_t _sourceWidth;
  uint16_t _sourceHeight;
  int16_t _playButtonUnderlayX;
  int16_t _playButtonUnderlayY;
  uint16_t _playButtonUnderlay[PlaybackControlUnderlaySize * PlaybackControlUnderlaySize];

  void resetView();
  void requestClose();
  void setOrientation(Orientation orientation, bool configureStream = true);
  void restorePortraitOrientation();
  void requestViewMode(ViewMode mode, bool pauseAfterFrame = false);
  void applyViewMode(ViewMode mode, bool clearOutgoingChrome);
  void updateViewportForViewMode();
  uint16_t viewportHeightFor(ViewMode mode) const;
  void useEmbeddedErrorMode();
  void clearEmbeddedChrome();
  void capturePlayButtonUnderlay(const uint16_t *pixels, int16_t frameX, int16_t frameY,
                                 uint16_t frameWidth, uint16_t frameHeight);
  void restorePlayButtonUnderlay();
  void scheduleAutoFullScreen();
  void updateStream(uint32_t nowMs);
  void presentFrames();
  void handleSdCardEvent(SdCardEvent event);
  void draw(bool full = true);
  void drawEmbeddedChrome();
  void drawTopBar();
  void drawPlayButton();
  void drawProgress();
  void drawOrientationControl();
  void drawError();
  uint16_t viewportTop() const;
  bool landscape() const;
  bool orientationControlContains(int16_t x, int16_t y) const;
  bool playButtonContains(int16_t x, int16_t y) const;
  bool videoCanvasContains(int16_t x, int16_t y) const;
  void videoFrameRect(int16_t &x, int16_t &y, uint16_t &width, uint16_t &height) const;
};
