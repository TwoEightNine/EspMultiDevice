#pragma once

#include <Arduino.h>

#include "../../core/Screen.h"
#include "../../ui/UiKit.h"
#include "benchmark/BenchmarkWorker.h"

// Settings -> Benchmark. A 42-second procedural 3D show rendered on both CPU
// cores and presented through LCD_CAM DMA, followed by a score card. The
// screen owns drawing, touch, and presentation; BenchmarkWorker owns all
// rendering and timing.
class BenchmarkScreen : public Screen {
public:
  BenchmarkScreen(Navigator &navigator, TouchController &touch, DisplayController &display);

  void onEnter() override;
  bool releaseResources() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  enum class State : uint8_t { Intro, Running, Results, Error };

  static constexpr int16_t CanvasTop = UiKit::AppBarHeight;
  static constexpr int16_t IntroCanvasHeight = 192;
  static constexpr int16_t RunCanvasHeight = 384;
  static constexpr int16_t ScoreCanvasHeight = 144;
  static constexpr int16_t HudTop = CanvasTop + RunCanvasHeight;
  static constexpr int16_t ButtonX = UiKit::ContentPadding;
  static constexpr int16_t ButtonWidth = 320 - UiKit::ContentPadding * 2;
  static constexpr int16_t ButtonHeight = 48;
  static constexpr int16_t IntroButtonY = 408;
  static constexpr int16_t ResultsButtonY = 424;
  static constexpr int16_t ResultsListY = 252;
  static constexpr uint8_t NoTransferSlot = 0xFF;

  BenchmarkWorker _worker;
  State _state = State::Intro;
  bool _lastTouchDown = false;
  bool _closeRequested = false;
  uint8_t _transferSlot = NoTransferSlot;
  uint32_t _jobGeneration = 0;
  uint32_t _bestScore = 0;
  uint32_t _score = 0;
  bool _newBest = false;
  float _temperature = NAN;
  float _stageFps[BenchmarkWorker::StageCount] = {};

  // HUD state shown during a run.
  uint32_t _lastHudMs = 0;
  int8_t _hudStage = -1;
  int16_t _hudFps = -1;
  uint8_t _hudProgress = 0;
  bool _hudHighlight = false;
  uint32_t _secondCoreMs = 0;

  void startIntro();
  void startRun();
  void showResults(const BenchmarkWorker::Snapshot &snapshot);
  void showError(const __FlashStringHelper *message);

  void drawIntro();
  void drawRunChrome();
  void drawHud(const BenchmarkWorker::Snapshot &snapshot, uint32_t nowMs, bool force);
  void drawResults();

  void presentFrames();
  void finishTransfer();
  void requestClose();

  uint32_t loadBestScore();
  void saveBestScore(uint32_t score);
  static String formatNumber(uint32_t value);
  static bool buttonContains(const TouchPoint &point, int16_t y);
};
