#pragma once

#include <Arduino.h>

#include "../../core/Screen.h"
#include "../../services/audio/AudioSettings.h"
#include "../../services/audio/SpeakerController.h"
#include "../../services/storage/SdCardController.h"
#include "AudioCoverWorker.h"
#include "AudioSource.h"

// "Now playing" screen: cover tile, track text, seek bar, and a transport row
// with repeat, previous, play/pause, next, and a volume popover. Decoding and
// file access live in AudioSource and the cover picture in AudioCoverWorker;
// this class owns layout, touch, and the playback state machine.
class AudioPlayerScreen : public Screen {
public:
  AudioPlayerScreen(Navigator &navigator,
                    TouchController &touch,
                    DisplayController &display,
                    SdCardController &sdCard,
                    SpeakerController &speaker,
                    AudioSettings &audioSettings,
                    SecretStorage &secretStorage,
                    MediaCache &mediaCache);

  void onEnter(const ScreenData &data) override;
  void onExit() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;
  bool releaseResources() override;

private:
  enum class State : uint8_t {
    Ready,
    Playing,
    Paused,
    Finished,
    Error
  };

  // Kept for the screen visit only. Until the folder queue exists, All
  // repeats the single track just like One.
  enum class Repeat : uint8_t {
    Off,
    All,
    One
  };

  enum class Drag : uint8_t {
    None,
    Seek,
    Volume
  };

  SdCardController &_sdCard;
  SpeakerController &_speaker;
  AudioSettings &_audioSettings;
  AudioSource _source;
  AudioCoverWorker _cover;
  // The decoded tile, owned by the UI task once taken from the worker.
  uint16_t *_coverPixels;
  // Bumped for every open; results for an older one are dropped.
  uint32_t _coverGeneration;

  String _path;
  String _fileName;
  String _error;
  bool _secretAccess;
  bool _sdMounted;
  bool _pendingOpen;
  bool _lastTouchDown;
  State _state;
  Repeat _repeat;
  Drag _drag;
  float _dragFraction;
  uint8_t _dragVolume;
  bool _volumeOpen;
  bool _infoOpen;
  uint32_t _volumeTouchMs;
  uint32_t _loadingStartMs;
  uint32_t _lastDrawMs;
  uint32_t _finishAtMs;

  // What the seek row currently shows, for incremental repaints.
  bool _seekDrawn;
  bool _drawnThumb;
  uint16_t _drawnFill;
  float _drawnFraction;
  int32_t _drawnElapsed;
  int32_t _drawnRemaining;

  bool openSource();
  void onSourceLoaded();
  bool loading() const { return _pendingOpen || _source.loading(); }
  bool tagsPending() const { return _pendingOpen || _source.tagsParsing(); }
  bool playable() const;
  bool startPlayback();
  void pausePlayback();
  void finishPlayback();
  void restartTrack();
  void cycleRepeat();
  void fail(const String &message);
  void failFromSource();
  void stopSpeaker();
  void handleSdCardEvent(SdCardEvent event);
  void requestCover();
  void takeCover();
  void dropCover();

  void beginDrag(Drag drag, int16_t x);
  void updateDrag(int16_t x);
  void endDrag();
  float seekFractionAt(int16_t x) const;
  uint8_t volumeLevelAt(int16_t x) const;
  uint8_t currentVolume() const;
  uint32_t displayedFrame() const;

  void openVolume();
  void closeVolume();
  void openInfo();
  void closeInfo();

  void draw();
  void refresh();
  void drawCover();
  void drawTrackText();
  void drawSeekRow(bool force);
  void drawTimes(bool force);
  void drawTransport();
  void drawRepeatCell();
  void drawPreviousCell();
  void drawPlayButton();
  void drawNextCell();
  void drawVolumeCell();
  void drawVolumePanel();
  void drawVolumeSlider(int16_t previousLevel);
  void drawTrackInfo();

  String trackTitle() const;
  String albumLine() const;
  String timeText(uint32_t seconds) const;

  bool cellContains(int16_t centerX, int16_t x, int16_t y) const;
  bool playContains(int16_t x, int16_t y) const;
  bool seekContains(int16_t x, int16_t y) const;
  bool volumePanelContains(int16_t x, int16_t y) const;
  bool volumeSliderContains(int16_t x, int16_t y) const;
};
