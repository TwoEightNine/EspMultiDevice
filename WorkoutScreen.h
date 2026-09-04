#pragma once

#include <Arduino.h>

#include "AudioSettings.h"
#include "src/core/Screen.h"
#include "SpeakerController.h"

class WorkoutScreen : public Screen {
public:
  WorkoutScreen(Navigator &navigator,
                TouchController &touch,
                DisplayController &display,
                SpeakerController &speaker,
                AudioSettings &audioSettings);

  void onEnter() override;
  void onExit() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  enum class Mode : uint8_t { Setup, Workout };
  enum class Stage : uint8_t { Init, Workout, Rest };

  struct Button {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;

    bool contains(int16_t px, int16_t py) const {
      return px >= x && px < x + w && py >= y && py < y + h;
    }
  };

  struct ToneCue {
    uint16_t frequency;
    uint16_t durationMs;
  };

  static const uint16_t MinDurationSeconds = 5;
  static const uint16_t MaxDurationSeconds = 600;
  static const uint16_t DurationStepSeconds = 5;
  static const uint16_t InitDurationSeconds = 5;
  static const uint32_t ToneSampleRate = 16000;
  static const uint16_t ToneChunkSamples = 256;
  static const uint8_t ToneQueueCapacity = 8;

  SpeakerController &_speaker;
  AudioSettings &_audioSettings;
  Mode _mode;
  Stage _stage;
  uint16_t _workoutSeconds;
  uint16_t _restSeconds;
  uint32_t _cycleIndex;
  bool _paused;
  bool _totalStarted;
  bool _lastTouchDown;
  bool _audioAvailable;
  uint32_t _stageStartedMs;
  uint32_t _stageElapsedBeforePauseMs;
  uint32_t _totalStartedMs;
  uint32_t _totalElapsedBeforePauseMs;
  bool _workoutFrameDrawn;
  Stage _drawnStage;
  uint32_t _drawnCycleIndex;
  uint32_t _drawnTotalSeconds;
  uint16_t _drawnStageSeconds;
  uint8_t _drawnProgress;
  bool _drawnPaused;
  uint8_t _initToneSecond;
  bool _workoutFirstThirdTonePlayed;
  bool _workoutSecondThirdTonePlayed;
  bool _restHalfTonePlayed;
  uint8_t _restWarningMask;
  ToneCue _toneQueue[ToneQueueCapacity];
  uint8_t _toneQueueHead;
  uint8_t _toneQueueCount;
  uint32_t _toneSamplesRemaining;
  uint32_t _tonePhase;
  uint32_t _tonePhaseStep;
  int16_t _toneBuffer[ToneChunkSamples];
  Button _startButton;
  Button _workoutMinusButton;
  Button _workoutPlusButton;
  Button _restMinusButton;
  Button _restPlusButton;
  Button _volumeMinusButton;
  Button _volumePlusButton;
  Button _pauseButton;

  void layout();
  void draw();
  void drawSetup();
  void drawStepper(const String &label, uint16_t value, const Button &minus, const Button &plus);
  void drawStepperValue(uint16_t value, const Button &minus);
  void drawWorkoutStatic();
  void drawWorkoutDynamic(uint32_t nowMs, bool force);
  void drawStageSeconds(uint16_t seconds);
  void drawButton(const Button &button, const String &label, uint16_t fill);
  void startWorkout(uint32_t nowMs);
  void cancelWorkout();
  void togglePause(uint32_t nowMs);
  void advanceStages(uint32_t nowMs);
  void beginStage(Stage stage, uint32_t startedMs);
  void updateToneCues(uint32_t nowMs);
  void enqueueTone(uint16_t frequency, uint16_t durationMs);
  void serviceTone();
  void clearTones();
  uint32_t elapsedInStage(uint32_t nowMs) const;
  uint32_t totalElapsed(uint32_t nowMs) const;
  uint32_t stageDurationMs() const;
  const char *stageName() const;
  String formatTotalTime(uint32_t elapsedMs) const;
  uint16_t shownStageSeconds(uint32_t nowMs) const;
  uint8_t stageProgress(uint32_t nowMs) const;
  void resetStageToneCues();
};
