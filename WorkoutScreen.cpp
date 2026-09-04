#include "WorkoutScreen.h"

#include "src/app/Navigator.h"
#include "UiKit.h"

namespace {
const int16_t ButtonHeight = 48;
const int16_t ButtonMargin = 16;
const uint16_t PrimaryButtonColor = TFT_BLUE;
const uint16_t SecondaryButtonColor = TFT_DARKGREY;
const uint16_t StageProgressColor = TFT_CYAN;
}

WorkoutScreen::WorkoutScreen(Navigator &navigator,
                             TouchController &touch,
                             DisplayController &display,
                             SpeakerController &speaker,
                             AudioSettings &audioSettings)
  : Screen(navigator, touch, display),
    _speaker(speaker),
    _audioSettings(audioSettings),
    _mode(Mode::Setup),
    _stage(Stage::Init),
    _workoutSeconds(25),
    _restSeconds(10),
    _cycleIndex(1),
    _paused(false),
    _totalStarted(false),
    _lastTouchDown(false),
    _audioAvailable(true),
    _stageStartedMs(0),
    _stageElapsedBeforePauseMs(0),
    _totalStartedMs(0),
    _totalElapsedBeforePauseMs(0),
    _workoutFrameDrawn(false),
    _drawnStage(Stage::Init),
    _drawnCycleIndex(0),
    _drawnTotalSeconds(0xffffffffUL),
    _drawnStageSeconds(0xffff),
    _drawnProgress(0xff),
    _drawnPaused(false),
    _initToneSecond(0),
    _workoutFirstThirdTonePlayed(false),
    _workoutSecondThirdTonePlayed(false),
    _restHalfTonePlayed(false),
    _restWarningMask(0),
    _toneQueueHead(0),
    _toneQueueCount(0),
    _toneSamplesRemaining(0),
    _tonePhase(0),
    _tonePhaseStep(0) {
  layout();
}

void WorkoutScreen::onEnter() {
  _lastTouchDown = false;
  _mode = Mode::Setup;
  _paused = false;
  clearTones();
  draw();
}

void WorkoutScreen::onExit() {
  cancelWorkout();
}

void WorkoutScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

  if (UiKit::navigationIconContains(point.x, point.y)) {
    if (_mode == Mode::Workout) {
      cancelWorkout();
      drawSetup();
    } else {
      _navigator.pop();
    }
    return;
  }

  if (_mode == Mode::Setup) {
    if (_startButton.contains(point.x, point.y)) {
      startWorkout(millis());
    } else if (_workoutMinusButton.contains(point.x, point.y) && _workoutSeconds > MinDurationSeconds) {
      _workoutSeconds -= DurationStepSeconds;
      drawStepperValue(_workoutSeconds, _workoutMinusButton);
    } else if (_workoutPlusButton.contains(point.x, point.y) && _workoutSeconds < MaxDurationSeconds) {
      _workoutSeconds += DurationStepSeconds;
      drawStepperValue(_workoutSeconds, _workoutMinusButton);
    } else if (_restMinusButton.contains(point.x, point.y) && _restSeconds > MinDurationSeconds) {
      _restSeconds -= DurationStepSeconds;
      drawStepperValue(_restSeconds, _restMinusButton);
    } else if (_restPlusButton.contains(point.x, point.y) && _restSeconds < MaxDurationSeconds) {
      _restSeconds += DurationStepSeconds;
      drawStepperValue(_restSeconds, _restMinusButton);
    } else if (_volumeMinusButton.contains(point.x, point.y) &&
               _audioSettings.volume() > AudioSettings::MinVolume) {
      _audioSettings.setVolume(_audioSettings.volume() - 1);
      drawStepperValue(_audioSettings.volume(), _volumeMinusButton);
    } else if (_volumePlusButton.contains(point.x, point.y) &&
               _audioSettings.volume() < AudioSettings::MaxVolume) {
      _audioSettings.setVolume(_audioSettings.volume() + 1);
      drawStepperValue(_audioSettings.volume(), _volumeMinusButton);
    }
    return;
  }

  if (_pauseButton.contains(point.x, point.y)) {
    togglePause(millis());
  }
}

void WorkoutScreen::tick(uint32_t nowMs) {
  if (_mode != Mode::Workout) return;

  if (!_paused) {
    advanceStages(nowMs);
    updateToneCues(nowMs);
    serviceTone();
  }

  drawWorkoutDynamic(nowMs, false);
}

void WorkoutScreen::layout() {
  const int16_t width = _display.width();
  const int16_t centerX = width / 2;
  const int16_t stepperButtonWidth = 72;
  const int16_t stepperButtonHeight = 42;

  _workoutMinusButton = { (int16_t)(centerX - 142), 72, stepperButtonWidth, stepperButtonHeight };
  _workoutPlusButton = { (int16_t)(centerX + 70), 72, stepperButtonWidth, stepperButtonHeight };
  _restMinusButton = { (int16_t)(centerX - 142), 132, stepperButtonWidth, stepperButtonHeight };
  _restPlusButton = { (int16_t)(centerX + 70), 132, stepperButtonWidth, stepperButtonHeight };
  _volumeMinusButton = { (int16_t)(centerX - 142), 192, stepperButtonWidth, stepperButtonHeight };
  _volumePlusButton = { (int16_t)(centerX + 70), 192, stepperButtonWidth, stepperButtonHeight };
  _startButton = { ButtonMargin, (int16_t)(_display.height() - ButtonHeight - 16),
                   (int16_t)(width - ButtonMargin * 2), ButtonHeight };
  _pauseButton = { ButtonMargin, (int16_t)(_display.height() - ButtonHeight - 16),
                   (int16_t)(width - ButtonMargin * 2), ButtonHeight };
}

void WorkoutScreen::draw() {
  layout();
  if (_mode == Mode::Setup) drawSetup();
  else {
    drawWorkoutStatic();
    drawWorkoutDynamic(millis(), true);
  }
}

void WorkoutScreen::drawSetup() {
  if (!_display.isReady()) return;

  layout();
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Workout"));
  drawStepper(F("workout"), _workoutSeconds, _workoutMinusButton, _workoutPlusButton);
  drawStepper(F("rest"), _restSeconds, _restMinusButton, _restPlusButton);
  drawStepper(F("volume"), _audioSettings.volume(), _volumeMinusButton, _volumePlusButton);
  drawButton(_startButton, F("Start"), PrimaryButtonColor);
}

void WorkoutScreen::drawStepper(const String &label,
                                uint16_t value,
                                const Button &minus,
                                const Button &plus) {
  TFT_eSPI &tft = _display.tft();
  const int16_t centerX = _display.width() / 2;
  const int16_t subtitleY = minus.y - 12;
  const int16_t buttonCenterY = minus.y + minus.h / 2;

  UiText::draw(tft, label, centerX, subtitleY, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  drawButton(minus, F("-"), SecondaryButtonColor);
  drawButton(plus, F("+"), SecondaryButtonColor);
  drawStepperValue(value, minus);
}

void WorkoutScreen::drawStepperValue(uint16_t value, const Button &minus) {
  if (!_display.isReady()) return;

  TFT_eSPI &tft = _display.tft();
  const int16_t centerX = _display.width() / 2;
  const int16_t buttonCenterY = minus.y + minus.h / 2;
  // The value is isolated between the two fixed +/- targets, so changing it
  // cannot disturb an in-progress touch on either control.
  tft.fillRect(centerX - 66, minus.y, 132, minus.h, TFT_BLACK);
  UiText::draw(tft, String(value), centerX, buttonCenterY, TFT_WHITE, TFT_BLACK,
               UiText::Size::Title, MC_DATUM);
}

void WorkoutScreen::drawWorkoutStatic() {
  if (!_display.isReady()) return;

  layout();
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Workout"));
  _workoutFrameDrawn = false;
}

void WorkoutScreen::drawWorkoutDynamic(uint32_t nowMs, bool force) {
  if (!_display.isReady() || _mode != Mode::Workout) return;
  if (!_workoutFrameDrawn) force = true;

  TFT_eSPI &tft = _display.tft();
  const uint32_t totalSeconds = totalElapsed(nowMs) / 1000UL;
  const uint16_t currentStageSeconds = shownStageSeconds(nowMs);
  const uint8_t currentProgress = stageProgress(nowMs);
  const bool stageChanged = force || _drawnStage != _stage;

  if (force || _drawnTotalSeconds != totalSeconds) {
    tft.fillRect(0, 54, _display.width(), 18, TFT_BLACK);
    String total = F("Total ");
    total += formatTotalTime(totalSeconds * 1000UL);
    UiText::draw(tft, total, _display.width() / 2, 63, TFT_LIGHTGREY, TFT_BLACK,
                 UiText::Size::Compact, MC_DATUM);
  }

  if (force || _drawnCycleIndex != _cycleIndex) {
    tft.fillRect(0, 73, _display.width(), 22, TFT_BLACK);
    String cycle = F("Cycle ");
    cycle += _cycleIndex;
    UiText::draw(tft, cycle, _display.width() / 2, 84, TFT_LIGHTGREY, TFT_BLACK,
                 UiText::Size::Content, MC_DATUM);
  }

  if (stageChanged) {
    tft.fillRect(0, 97, _display.width(), 24, TFT_BLACK);
    UiText::draw(tft, String(stageName()), _display.width() / 2, 109, TFT_WHITE, TFT_BLACK,
                 UiText::Size::Content, MC_DATUM);
  }

  if (force || _drawnStageSeconds != currentStageSeconds) {
    drawStageSeconds(currentStageSeconds);
  }

  if (force || _drawnProgress != currentProgress) {
    const int16_t barX = ButtonMargin;
    const int16_t barY = 205;
    const int16_t barWidth = _display.width() - ButtonMargin * 2;
    if (force || stageChanged) {
      UiKit::drawProgressBar(tft, barX, barY, barWidth, 14, currentProgress, false,
                             StageProgressColor, TFT_DARKGREY, TFT_BLACK, true);
    } else {
      UiKit::updateProgressBar(tft, barX, barY, barWidth, 14, _drawnProgress, currentProgress,
                               StageProgressColor, TFT_BLACK);
    }
  }

  if (force || _drawnPaused != _paused) {
    drawButton(_pauseButton, _paused ? F("Continue") : F("Pause"),
               _paused ? PrimaryButtonColor : SecondaryButtonColor);
  }

  _workoutFrameDrawn = true;
  _drawnStage = _stage;
  _drawnCycleIndex = _cycleIndex;
  _drawnTotalSeconds = totalSeconds;
  _drawnStageSeconds = currentStageSeconds;
  _drawnProgress = currentProgress;
  _drawnPaused = _paused;
}

void WorkoutScreen::drawStageSeconds(uint16_t seconds) {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, 122, _display.width(), 74, TFT_BLACK);
  tft.setTextFont(8);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawNumber(seconds, _display.width() / 2, 157);
  tft.setTextFont(1);
  tft.setTextDatum(TL_DATUM);
}

void WorkoutScreen::drawButton(const Button &button, const String &label, uint16_t fill) {
  TFT_eSPI &tft = _display.tft();
  tft.fillSmoothRoundRect(button.x, button.y, button.w, button.h, 7, fill, TFT_BLACK);
  UiText::draw(tft, label, button.x + button.w / 2, button.y + button.h / 2,
               TFT_WHITE, fill, UiText::Size::Content, MC_DATUM);
}

void WorkoutScreen::startWorkout(uint32_t nowMs) {
  clearTones();
  _mode = Mode::Workout;
  _stage = Stage::Init;
  _cycleIndex = 1;
  _paused = false;
  _totalStarted = false;
  _stageStartedMs = nowMs;
  _stageElapsedBeforePauseMs = 0;
  _totalStartedMs = 0;
  _totalElapsedBeforePauseMs = 0;
  _audioAvailable = _speaker.start(ToneSampleRate);
  resetStageToneCues();
  _workoutFrameDrawn = false;
  drawWorkoutStatic();
  drawWorkoutDynamic(millis(), true);
}

void WorkoutScreen::cancelWorkout() {
  _paused = false;
  _totalStarted = false;
  _stageElapsedBeforePauseMs = 0;
  _totalElapsedBeforePauseMs = 0;
  clearTones();
  _mode = Mode::Setup;
}

void WorkoutScreen::togglePause(uint32_t nowMs) {
  if (_paused) {
    _stageStartedMs = nowMs;
    if (_totalStarted) _totalStartedMs = nowMs;
    _paused = false;
    _audioAvailable = _speaker.start(ToneSampleRate);
  } else {
    _stageElapsedBeforePauseMs = elapsedInStage(nowMs);
    if (_totalStarted) _totalElapsedBeforePauseMs = totalElapsed(nowMs);
    _paused = true;
    clearTones();
  }
  drawWorkoutDynamic(nowMs, false);
}

void WorkoutScreen::advanceStages(uint32_t nowMs) {
  while (elapsedInStage(nowMs) >= stageDurationMs()) {
    const uint32_t overflowMs = elapsedInStage(nowMs) - stageDurationMs();
    const uint32_t nextStartedMs = nowMs - overflowMs;
    if (_stage == Stage::Init) {
      _totalStarted = true;
      _totalElapsedBeforePauseMs = 0;
      _totalStartedMs = nextStartedMs;
      beginStage(Stage::Workout, nextStartedMs);
    } else if (_stage == Stage::Workout) {
      beginStage(Stage::Rest, nextStartedMs);
    } else {
      _cycleIndex++;
      beginStage(Stage::Workout, nextStartedMs);
    }
  }
}

void WorkoutScreen::beginStage(Stage stage, uint32_t startedMs) {
  _stage = stage;
  _stageStartedMs = startedMs;
  _stageElapsedBeforePauseMs = 0;
  resetStageToneCues();
  if (stage == Stage::Workout || stage == Stage::Rest) {
    enqueueTone(1000, 500);
  }
}

void WorkoutScreen::updateToneCues(uint32_t nowMs) {
  const uint32_t elapsedMs = elapsedInStage(nowMs);
  const uint32_t durationMs = stageDurationMs();

  if (_stage == Stage::Init) {
    const uint8_t completedSeconds = (uint8_t)(elapsedMs / 1000UL);
    while (_initToneSecond < completedSeconds && _initToneSecond < InitDurationSeconds - 1) {
      _initToneSecond++;
      enqueueTone(500, 125);
    }
    return;
  }

  if (_stage == Stage::Workout) {
    const uint32_t firstThirdMs = durationMs / 3UL;
    const uint32_t secondThirdMs = (durationMs * 2UL) / 3UL;
    if (!_workoutFirstThirdTonePlayed && elapsedMs >= firstThirdMs) {
      _workoutFirstThirdTonePlayed = true;
      enqueueTone(1500, 125);
    }
    if (!_workoutSecondThirdTonePlayed && elapsedMs >= secondThirdMs) {
      _workoutSecondThirdTonePlayed = true;
      enqueueTone(1500, 125);
    }
    return;
  }

  if (!_restHalfTonePlayed && elapsedMs >= durationMs / 2UL) {
    _restHalfTonePlayed = true;
    enqueueTone(500, 125);
  }
  const uint8_t warnings[] = { 3, 2, 1 };
  for (uint8_t i = 0; i < sizeof(warnings); i++) {
    const uint8_t warning = warnings[i];
    const uint8_t bit = (uint8_t)(1U << i);
    const uint32_t warningAtMs = durationMs - (uint32_t)warning * 1000UL;
    if (!(_restWarningMask & bit) && elapsedMs >= warningAtMs) {
      _restWarningMask |= bit;
      enqueueTone(500, 125);
    }
  }
}

void WorkoutScreen::enqueueTone(uint16_t frequency, uint16_t durationMs) {
  if (!_audioAvailable || _toneQueueCount >= ToneQueueCapacity) return;
  const uint8_t tail = (uint8_t)((_toneQueueHead + _toneQueueCount) % ToneQueueCapacity);
  _toneQueue[tail] = { frequency, durationMs };
  _toneQueueCount++;
}

void WorkoutScreen::serviceTone() {
  if (!_audioAvailable) return;

  if (_toneSamplesRemaining == 0 && _toneQueueCount > 0) {
    const ToneCue cue = _toneQueue[_toneQueueHead];
    _toneQueueHead = (uint8_t)((_toneQueueHead + 1) % ToneQueueCapacity);
    _toneQueueCount--;
    if (!_speaker.isReady()) {
      _audioAvailable = false;
      _toneQueueCount = 0;
      return;
    }
    _toneSamplesRemaining = (uint32_t)cue.durationMs * ToneSampleRate / 1000UL;
    _tonePhase = 0;
    _tonePhaseStep = ((uint32_t)cue.frequency << 16) / ToneSampleRate;
  }

  if (_toneSamplesRemaining == 0) return;
  uint16_t count = _toneSamplesRemaining > ToneChunkSamples
    ? ToneChunkSamples : (uint16_t)_toneSamplesRemaining;
  for (uint16_t i = 0; i < count; i++) {
    _toneBuffer[i] = (_tonePhase & 0x8000UL) ? 11000 : -11000;
    _tonePhase += _tonePhaseStep;
  }
  const size_t written = _speaker.writePcm16(_toneBuffer, count, 0);
  if (written == 0) return;

  _toneSamplesRemaining -= written;
  _tonePhase -= _tonePhaseStep * (count - written);
}

void WorkoutScreen::clearTones() {
  _toneQueueHead = 0;
  _toneQueueCount = 0;
  _toneSamplesRemaining = 0;
  _tonePhase = 0;
  _tonePhaseStep = 0;
  _speaker.stop();
}

uint32_t WorkoutScreen::elapsedInStage(uint32_t nowMs) const {
  return _stageElapsedBeforePauseMs + (_paused ? 0 : nowMs - _stageStartedMs);
}

uint32_t WorkoutScreen::totalElapsed(uint32_t nowMs) const {
  if (!_totalStarted) return 0;
  return _totalElapsedBeforePauseMs + (_paused ? 0 : nowMs - _totalStartedMs);
}

uint32_t WorkoutScreen::stageDurationMs() const {
  if (_stage == Stage::Init) return (uint32_t)InitDurationSeconds * 1000UL;
  if (_stage == Stage::Workout) return (uint32_t)_workoutSeconds * 1000UL;
  return (uint32_t)_restSeconds * 1000UL;
}

const char *WorkoutScreen::stageName() const {
  if (_stage == Stage::Init) return "Init";
  if (_stage == Stage::Workout) return "Workout";
  return "Rest";
}

String WorkoutScreen::formatTotalTime(uint32_t elapsedMs) const {
  const uint32_t seconds = elapsedMs / 1000UL;
  char buffer[12];
  if (seconds >= 3600UL) {
    snprintf(buffer, sizeof(buffer), "%lu:%02lu:%02lu",
             (unsigned long)(seconds / 3600UL),
             (unsigned long)((seconds / 60UL) % 60UL),
             (unsigned long)(seconds % 60UL));
  } else {
    snprintf(buffer, sizeof(buffer), "%02lu:%02lu",
             (unsigned long)(seconds / 60UL), (unsigned long)(seconds % 60UL));
  }
  return String(buffer);
}

uint16_t WorkoutScreen::shownStageSeconds(uint32_t nowMs) const {
  const uint32_t elapsedMs = elapsedInStage(nowMs);
  const uint32_t durationMs = stageDurationMs();
  if (_stage == Stage::Init) {
    const uint32_t remainingMs = elapsedMs >= durationMs ? 0 : durationMs - elapsedMs;
    return (uint16_t)((remainingMs + 999UL) / 1000UL);
  }
  return (uint16_t)(elapsedMs / 1000UL);
}

uint8_t WorkoutScreen::stageProgress(uint32_t nowMs) const {
  const uint32_t durationMs = stageDurationMs();
  const uint32_t elapsedMs = elapsedInStage(nowMs);
  if (elapsedMs >= durationMs) return 100;
  return (uint8_t)((elapsedMs * 100UL) / durationMs);
}

void WorkoutScreen::resetStageToneCues() {
  _initToneSecond = 0;
  _workoutFirstThirdTonePlayed = false;
  _workoutSecondThirdTonePlayed = false;
  _restHalfTonePlayed = false;
  _restWarningMask = 0;
}
