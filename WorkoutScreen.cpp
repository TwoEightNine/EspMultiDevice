#include "WorkoutScreen.h"

#include "Navigator.h"
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
                             SpeakerController &speaker)
  : Screen(navigator, touch, display),
    _speaker(speaker),
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
    _lastDrawMs(0),
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
      drawSetup();
    } else if (_workoutPlusButton.contains(point.x, point.y) && _workoutSeconds < MaxDurationSeconds) {
      _workoutSeconds += DurationStepSeconds;
      drawSetup();
    } else if (_restMinusButton.contains(point.x, point.y) && _restSeconds > MinDurationSeconds) {
      _restSeconds -= DurationStepSeconds;
      drawSetup();
    } else if (_restPlusButton.contains(point.x, point.y) && _restSeconds < MaxDurationSeconds) {
      _restSeconds += DurationStepSeconds;
      drawSetup();
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

  if (_lastDrawMs == 0 || nowMs - _lastDrawMs >= 100) {
    drawWorkout(false);
  }
}

void WorkoutScreen::layout() {
  const int16_t width = _display.width();
  const int16_t centerX = width / 2;
  const int16_t stepperButtonWidth = 72;
  const int16_t stepperButtonHeight = 42;

  _startButton = { ButtonMargin, 64, (int16_t)(width - ButtonMargin * 2), ButtonHeight };
  _workoutMinusButton = { (int16_t)(centerX - 142), 170, stepperButtonWidth, stepperButtonHeight };
  _workoutPlusButton = { (int16_t)(centerX + 70), 170, stepperButtonWidth, stepperButtonHeight };
  _restMinusButton = { (int16_t)(centerX - 142), 242, stepperButtonWidth, stepperButtonHeight };
  _restPlusButton = { (int16_t)(centerX + 70), 242, stepperButtonWidth, stepperButtonHeight };
  _pauseButton = { ButtonMargin, (int16_t)(_display.height() - ButtonHeight - 16),
                   (int16_t)(width - ButtonMargin * 2), ButtonHeight };
}

void WorkoutScreen::draw() {
  layout();
  if (_mode == Mode::Setup) drawSetup();
  else drawWorkout(true);
}

void WorkoutScreen::drawSetup() {
  if (!_display.isReady()) return;

  layout();
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Workout"));
  drawButton(_startButton, F("Start"), PrimaryButtonColor);
  drawStepper(F("workout"), _workoutSeconds, _workoutMinusButton, _workoutPlusButton);
  drawStepper(F("rest"), _restSeconds, _restMinusButton, _restPlusButton);
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
  UiText::draw(tft, String(value), centerX, buttonCenterY, TFT_WHITE, TFT_BLACK,
               UiText::Size::Title, MC_DATUM);
}

void WorkoutScreen::drawWorkout(bool staticFrame) {
  if (!_display.isReady()) return;

  layout();
  TFT_eSPI &tft = _display.tft();
  if (staticFrame) {
    tft.fillScreen(TFT_BLACK);
    UiKit::drawTopAppBar(tft, _display.width(), F("Workout"));
  }

  const int16_t contentBottom = _pauseButton.y - 12;
  tft.fillRect(0, UiKit::AppBarHeight, _display.width(),
               contentBottom - UiKit::AppBarHeight, TFT_BLACK);

  const uint32_t nowMs = millis();
  const uint32_t stageElapsedMs = elapsedInStage(nowMs);
  const uint32_t durationMs = stageDurationMs();
  const uint32_t shownStageMs = _stage == Stage::Init && stageElapsedMs < durationMs
    ? durationMs - stageElapsedMs : stageElapsedMs;

  String total = F("Total ");
  total += formatTime(totalElapsed(nowMs), true);
  UiText::draw(tft, total, _display.width() / 2, 76, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);

  String cycle = F("Cycle ");
  cycle += _cycleIndex;
  UiText::draw(tft, cycle, _display.width() / 2, 104, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Content, MC_DATUM);

  UiText::draw(tft, String(stageName()), _display.width() / 2, 143, TFT_WHITE, TFT_BLACK,
               UiText::Size::Title, MC_DATUM);
  UiText::draw(tft, formatTime(shownStageMs), _display.width() / 2, 178, TFT_WHITE, TFT_BLACK,
               UiText::Size::Title, MC_DATUM);

  const int16_t barX = ButtonMargin;
  const int16_t barY = 205;
  const int16_t barWidth = _display.width() - ButtonMargin * 2;
  UiKit::drawProgressBar(tft, barX, barY, barWidth, 14, stageProgress(nowMs), false,
                         StageProgressColor, TFT_DARKGREY, TFT_BLACK, true);

  drawButton(_pauseButton, _paused ? F("Continue") : F("Pause"),
             _paused ? PrimaryButtonColor : SecondaryButtonColor);
  _lastDrawMs = nowMs;
}

void WorkoutScreen::drawButton(const Button &button, const String &label, uint16_t fill) {
  TFT_eSPI &tft = _display.tft();
  tft.fillRoundRect(button.x, button.y, button.w, button.h, 7, fill);
  tft.drawRoundRect(button.x, button.y, button.w, button.h, 7, TFT_WHITE);
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
  _lastDrawMs = 0;
  drawWorkout(true);
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
  drawWorkout(false);
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

String WorkoutScreen::formatTime(uint32_t elapsedMs, bool hoursWhenNeeded) const {
  const uint32_t seconds = elapsedMs / 1000UL;
  char buffer[12];
  if (hoursWhenNeeded && seconds >= 3600UL) {
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
