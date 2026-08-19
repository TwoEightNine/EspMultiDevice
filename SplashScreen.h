#pragma once


#include <Arduino.h>

#include "Screen.h"

class SplashScreen : public Screen {
public:
    SplashScreen(Navigator &navigator,
               TouchController &touch,
               DisplayController &display);

    void onEnter() override;
    void handleTouch(const TouchPoint &point) override;
    void tick(uint32_t nowMs) override;

private:
    unsigned long enteredMillis = 0;
    // for animations
    int currentFrame = -1;
    int currentFrameStartedMillis = 0;
};
