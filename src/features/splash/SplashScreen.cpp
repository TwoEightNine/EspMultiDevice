#include "SplashScreen.h"
#include "../../app/Navigator.h"
#include "../../services/network/WiFiController.h"
#include "../../ui/UiKit.h"
#include "../../ui/logo.h"

namespace {
constexpr uint32_t SplashDurationMs = 3000;
}

SplashScreen::SplashScreen(Navigator &navigator,
                           TouchController &touch,
                           DisplayController &display,
                           WiFiController &wifi)
  : Screen(navigator, touch, display),
    _wifi(wifi) {
}

void SplashScreen::onEnter() {
  _transitioned = false;
  _display.clear(TFT_BLACK);
  const int16_t startX = static_cast<int16_t>(
    (static_cast<int32_t>(_display.width()) - logo_width) / 2);
  const int16_t startY = static_cast<int16_t>(
    (static_cast<int32_t>(_display.height()) - logo_height) / 2);
  UiKit::drawRgba565Image(_display.tft(), startX, startY,
                          logo_width, logo_height, logo_colors, logo_alpha,
                          TFT_BLACK);

  // Reconnect saved credentials while the splash remains visible. Avoid an
  // unsolicited scan on devices that have not been configured yet.
  _wifi.setEnabled(true, false);
  _enteredMs = millis();
}

void SplashScreen::handleTouch(const TouchPoint &point) {
  (void)point;
}

void SplashScreen::tick(uint32_t nowMs) {
  if (_transitioned || nowMs - _enteredMs < SplashDurationMs) return;

  _transitioned = true;
  _navigator.replace(ScreenId::Home);
}
