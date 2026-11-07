#include "BenchmarkScreen.h"

#include <Preferences.h>
#include <math.h>

#include "../../app/Navigator.h"
#include "../../ui/UiText.h"

namespace {

constexpr char PreferencesNamespace[] = "bench";
constexpr char BestScoreKey[] = "best";
constexpr uint32_t HudIntervalMs = 250;
constexpr uint32_t SecondCoreBannerMs = 2500;
constexpr uint8_t SecondCoreStage = 3;
constexpr uint16_t HudColor = UiKit::AppBarColor;
constexpr uint16_t TrackColor = 0x2104;  // #222222
constexpr uint16_t AccentColor = TFT_CYAN;
constexpr uint16_t ButtonColor = TFT_BLUE;
constexpr int16_t HudLabelWidth = 216;
constexpr int16_t HudRowCenter = 14;
constexpr int16_t HudBarY = 34;
constexpr int16_t HudBarHeight = 8;

String formatFps(float fps) {
  if (fps <= 0.0f) return String(F("--"));
  return fps < 10.0f ? String(fps, 1) : String((int)(fps + 0.5f));
}

}  // namespace

BenchmarkScreen::BenchmarkScreen(Navigator &navigator, TouchController &touch, DisplayController &display)
  : Screen(navigator, touch, display) {
}

void BenchmarkScreen::onEnter() {
  _lastTouchDown = false;
  _closeRequested = false;
  _transferSlot = NoTransferSlot;
  _bestScore = loadBestScore();
  if (!_worker.started() && !_worker.begin()) {
    showError(F("Not enough free memory to run."));
    return;
  }
  startIntro();
}

bool BenchmarkScreen::releaseResources() {
  finishTransfer();
  _worker.cancel();
  if (!_worker.end()) return false;
  _display.endDmaImages();
  _closeRequested = false;
  return true;
}

void BenchmarkScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

  if (UiKit::navigationIconContains(point.x, point.y)) {
    requestClose();
    return;
  }
  if ((_state == State::Intro && buttonContains(point, IntroButtonY)) ||
      (_state == State::Results && buttonContains(point, ResultsButtonY))) {
    startRun();
  }
}

void BenchmarkScreen::tick(uint32_t nowMs) {
  if (_closeRequested) {
    // Release waits for both render tasks to stop; retry until it does.
    if (_navigator.pop()) return;
  }
  if (_state == State::Error) return;

  presentFrames();

  const BenchmarkWorker::Snapshot snapshot = _worker.snapshot();
  if (snapshot.generation == _jobGeneration && _state == State::Running) {
    if (snapshot.failed) {
      showError(F("The benchmark ran out of memory."));
      return;
    }
    if (snapshot.finished && snapshot.mode == BenchmarkWorker::Mode::Run) {
      showResults(snapshot);
      return;
    }
    // Repaint the HUD between frames so it never waits on an active transfer.
    if (_transferSlot == NoTransferSlot) drawHud(snapshot, nowMs, false);
  }
  // While rendering, let the helper task on this core take the rest of the
  // tick; touch polling still runs every millisecond.
  if (snapshot.active && snapshot.generation == _jobGeneration) vTaskDelay(1);
}

void BenchmarkScreen::startIntro() {
  finishTransfer();
  _state = State::Intro;
  drawIntro();
  _worker.startPreview(IntroCanvasHeight);
  _jobGeneration = _worker.snapshot().generation;
}

void BenchmarkScreen::startRun() {
  finishTransfer();
  _state = State::Running;
  _hudStage = -1;
  _hudFps = -1;
  _hudProgress = 0;
  _hudHighlight = false;
  _secondCoreMs = 0;
  drawRunChrome();
  _worker.startRun(RunCanvasHeight);
  const BenchmarkWorker::Snapshot snapshot = _worker.snapshot();
  _jobGeneration = snapshot.generation;
  drawHud(snapshot, millis(), true);
}

void BenchmarkScreen::showResults(const BenchmarkWorker::Snapshot &snapshot) {
  finishTransfer();
  for (uint8_t i = 0; i < BenchmarkWorker::StageCount; ++i) _stageFps[i] = snapshot.stageFps[i];
  _score = BenchmarkWorker::score(_stageFps);
  _temperature = temperatureRead();
  _newBest = _score > _bestScore;
  if (_newBest) {
    _bestScore = _score;
    saveBestScore(_score);
  }
  _state = State::Results;
  drawResults();
  _worker.startScore(ScoreCanvasHeight, _score);
  _jobGeneration = _worker.snapshot().generation;
}

void BenchmarkScreen::showError(const __FlashStringHelper *message) {
  finishTransfer();
  _worker.cancel();
  _state = State::Error;
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Benchmark"));
  UiText::draw(tft, message, _display.width() / 2, 240, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Content, MC_DATUM);
}

void BenchmarkScreen::requestClose() {
  _closeRequested = true;
  _worker.cancel();
  _navigator.pop();
}

void BenchmarkScreen::drawIntro() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t centerX = _display.width() / 2;
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Benchmark"));

  UiText::draw(tft, F("How fast is it?"), centerX, 266, TFT_WHITE, TFT_BLACK,
               UiText::Size::Title, MC_DATUM);
  String line = F("A ");
  line += BenchmarkWorker::totalRunMs() / 1000;
  line += F("-second 3D show that pushes");
  UiText::draw(tft, line, centerX, 300, TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Content, MC_DATUM);
  UiText::draw(tft, F("the processor and the screen."), centerX, 322, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Content, MC_DATUM);

  String best;
  if (_bestScore) {
    best = F("Best score: ");
    best += formatNumber(_bestScore);
  } else {
    best = F("No score yet");
  }
  UiText::draw(tft, best, centerX, 360, _bestScore ? AccentColor : TFT_DARKGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  UiKit::drawFilledButton(tft, ButtonX, IntroButtonY, ButtonWidth, ButtonHeight, F("Start"), ButtonColor);
}

void BenchmarkScreen::drawRunChrome() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Benchmark"));
  tft.fillRect(0, HudTop, _display.width(), _display.height() - HudTop, HudColor);
  UiKit::drawProgressBar(tft, ButtonX, HudTop + HudBarY, ButtonWidth, HudBarHeight, 0, false,
                         AccentColor, TFT_DARKGREY, TrackColor);
}

void BenchmarkScreen::drawHud(const BenchmarkWorker::Snapshot &snapshot, uint32_t nowMs, bool force) {
  if (!force && nowMs - _lastHudMs < HudIntervalMs) return;
  if (!_display.isReady()) return;
  _lastHudMs = nowMs;
  TFT_eSPI &tft = _display.tft();
  const int16_t rowY = HudTop + HudRowCenter;

  const int8_t stage = (int8_t)snapshot.stage;
  if (stage == SecondCoreStage && _hudStage != SecondCoreStage) _secondCoreMs = nowMs;
  const bool highlight = stage == SecondCoreStage && nowMs - _secondCoreMs < SecondCoreBannerMs;
  if (force || stage != _hudStage || highlight != _hudHighlight) {
    _hudStage = stage;
    _hudHighlight = highlight;
    tft.fillRect(0, HudTop, HudLabelWidth, HudBarY - 2, HudColor);
    const String label = highlight ? String(F("Second core joined!")) : String(BenchmarkWorker::stageName(stage));
    UiText::draw(tft, label, UiKit::ContentPadding, rowY, highlight ? AccentColor : TFT_WHITE, HudColor,
                 UiText::Size::Content, ML_DATUM);
  }

  const int16_t fps = snapshot.liveFps > 0.0f ? (int16_t)(snapshot.liveFps + 0.5f) : 0;
  if (force || fps != _hudFps) {
    _hudFps = fps;
    const int16_t unitX = _display.width() - UiKit::ContentPadding;
    tft.fillRect(HudLabelWidth, HudTop, _display.width() - HudLabelWidth, HudBarY - 2, HudColor);
    UiText::draw(tft, F("FPS"), unitX, rowY + 1, TFT_LIGHTGREY, HudColor, UiText::Size::Compact, MR_DATUM);
    const int16_t numberX = unitX - UiText::measure(String(F("FPS")), UiText::Size::Compact) - 6;
    UiText::draw(tft, fps ? String(fps) : String(F("--")), numberX, rowY, TFT_WHITE, HudColor,
                 UiText::Size::Title, MR_DATUM);
  }

  if (snapshot.progress != _hudProgress) {
    UiKit::updateProgressBar(tft, ButtonX, HudTop + HudBarY, ButtonWidth, HudBarHeight, _hudProgress,
                             snapshot.progress, AccentColor, TrackColor);
    _hudProgress = snapshot.progress;
  }
}

void BenchmarkScreen::drawResults() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t width = _display.width();
  const int16_t centerX = width / 2;
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, width, F("Benchmark"));

  String rating;
  uint16_t ratingColor;
  if (_score >= 1100) {
    rating = F("Outstanding!");
    ratingColor = TFT_CYAN;
  } else if (_score >= 900) {
    rating = F("Excellent");
    ratingColor = TFT_GREEN;
  } else if (_score >= 700) {
    rating = F("Good");
    ratingColor = TFT_YELLOW;
  } else {
    rating = F("Slower than usual");
    ratingColor = TFT_ORANGE;
  }
  if (_newBest) rating += F(" \xC2\xB7 New best!");
  UiText::draw(tft, rating, centerX, CanvasTop + ScoreCanvasHeight + 18, ratingColor, TFT_BLACK,
               UiText::Size::Title, MC_DATUM);

  String detail;
  if (!isnan(_temperature)) {
    detail += F("Chip ");
    detail += (int)(_temperature + 0.5f);
    detail += F("\xC2\xB0""C \xC2\xB7 ");
  }
  const float single = _stageFps[2];
  const float dual = _stageFps[3];
  if (single > 0.0f && dual > 0.0f) {
    detail += F("Two cores: \xC3\x97");
    detail += String(dual / single, 1);
    detail += F(" faster");
  } else if (_score < 700) {
    detail += F("Wi-Fi or the Assistant may be busy");
  }
  UiText::draw(tft, detail, centerX, CanvasTop + ScoreCanvasHeight + 44, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);

  const String fpsSuffix = F(" fps");
  int16_t y = ResultsListY;
  UiKit::drawListCell(tft, width, y, BenchmarkWorker::stageName(0), formatFps(_stageFps[0]) + fpsSuffix);
  y += UiKit::ListCellHeight;
  UiKit::drawListCell(tft, width, y, BenchmarkWorker::stageName(1), formatFps(_stageFps[1]) + fpsSuffix);
  y += UiKit::ListCellHeight;
  UiKit::drawListCell(tft, width, y, F("Knot \xC2\xB7 1 vs 2 cores"),
                      formatFps(single) + F(" / ") + formatFps(dual) + fpsSuffix);
  y += UiKit::ListCellHeight;
  UiKit::drawListCell(tft, width, y, BenchmarkWorker::stageName(4), formatFps(_stageFps[4]) + fpsSuffix);

  UiKit::drawFilledButton(tft, ButtonX, ResultsButtonY, ButtonWidth, ButtonHeight, F("Run again"), ButtonColor);
}

void BenchmarkScreen::presentFrames() {
  if (_transferSlot != NoTransferSlot) {
    if (_display.dmaImageActive()) return;
    finishTransfer();
  }
  if (_state == State::Running) {
    drawHud(_worker.snapshot(), millis(), false);
  }
  BenchmarkWorker::Frame frame;
  if (!_worker.takeFrame(frame)) return;
  if (_display.beginDmaImage(0, CanvasTop, BenchRaster::Width, frame.height, frame.pixels)) {
    _transferSlot = frame.slot;
    return;
  }
  _display.tft().pushImage(0, CanvasTop, BenchRaster::Width, frame.height, const_cast<uint16_t *>(frame.pixels));
  _worker.releaseFrame(frame.slot);
}

void BenchmarkScreen::finishTransfer() {
  if (_transferSlot == NoTransferSlot) return;
  _display.finishDmaImage();
  _worker.releaseFrame(_transferSlot);
  _transferSlot = NoTransferSlot;
}

uint32_t BenchmarkScreen::loadBestScore() {
  Preferences preferences;
  if (!preferences.begin(PreferencesNamespace, false)) return 0;
  const uint32_t best = preferences.getUInt(BestScoreKey, 0);
  preferences.end();
  return best;
}

void BenchmarkScreen::saveBestScore(uint32_t score) {
  Preferences preferences;
  if (!preferences.begin(PreferencesNamespace, false)) return;
  preferences.putUInt(BestScoreKey, score);
  preferences.end();
}

String BenchmarkScreen::formatNumber(uint32_t value) {
  String digits(value);
  String out;
  const int16_t length = digits.length();
  for (int16_t i = 0; i < length; ++i) {
    if (i > 0 && (length - i) % 3 == 0) out += ',';
    out += digits[i];
  }
  return out;
}

bool BenchmarkScreen::buttonContains(const TouchPoint &point, int16_t y) {
  return point.x >= ButtonX && point.x < ButtonX + ButtonWidth && point.y >= y && point.y < y + ButtonHeight;
}
