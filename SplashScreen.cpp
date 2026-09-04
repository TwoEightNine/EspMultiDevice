#include "SplashScreen.h"
#include "src/app/Navigator.h"
#include "WiFiController.h"
#include "logo.h"

#define SPLASH_DURATION_MS 3000

SplashScreen::SplashScreen(Navigator &navigator,
                       TouchController &touch,
                       DisplayController &display,
                       WiFiController &wifi)
  : Screen(navigator, touch, display),
    _wifi(wifi) {
}

void SplashScreen::onEnter() {
    enteredMillis = millis();
    uint16_t startX = (_display.width() - logo_WIDTH) / 2;
    uint16_t startY = (_display.height() - logo_HEIGHT) / 2;
    _display.tft().pushImage(startX, startY, logo_WIDTH, logo_HEIGHT, logo);
}

void SplashScreen::handleTouch(const TouchPoint &point) {

}

void SplashScreen::tick(uint32_t nowMs) {
    // animations
    // bool updateFrame = false;
    // if (currentFrame == -1) {
    //     currentFrame = 0;
    //     updateFrame = true;
    // }
    // if (millis() - currentFrameStartedMillis >= 100) {
    //     if (currentFrame == ANIMATIONS_FRAME_COUNT - 1) {
    //         _navigator.replace(ScreenId::Home);
    //     } else {
    //         currentFrame++;
    //         updateFrame = true;
    //     }
    // }
    // if (updateFrame) {
    //     _display.tft().pushImage(0, 0, TFT_ANIMATION_WIDTH, TFT_ANIMATION_HEIGHT, animations_frames[currentFrame]);
    // }

    // static logo
    if (millis() - enteredMillis < SPLASH_DURATION_MS) {

    } else if (enteredMillis != 0) {
        enteredMillis = 0;
        _navigator.replace(ScreenId::Home);
        // Reconnect to saved credentials after Home/SD startup, without an
        // unsolicited scan on an unconfigured device.
        _wifi.setEnabled(true, false);
    }
}
