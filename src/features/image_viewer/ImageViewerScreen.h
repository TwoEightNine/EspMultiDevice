#pragma once

#include <Arduino.h>

#include "../../core/Screen.h"
#include "ImageView.h"

class ImageViewerScreen : public Screen {
public:
  ImageViewerScreen(Navigator &navigator,
                    TouchController &touch,
                    DisplayController &display,
                    ImageView &view);

  void onEnter(const ScreenData &data) override;
  void onExit() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const int16_t EdgeTapWidth = 48;
  static const int16_t CloseTapSize = 48;

  ImageView &_view;
  String _previousImagePath;
  String _nextImagePath;
  bool _lastTouchDown;

  static void paintChrome(void *context);
  void drawChrome();
};
