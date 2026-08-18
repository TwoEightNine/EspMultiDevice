#include <Arduino.h>

#include "DisplayController.h"
#include "HomeScreen.h"
#include "Navigator.h"
#include "SdCardController.h"
#include "TouchController.h"

static const uint32_t SERIAL_BAUD = 115200;
static const uint8_t DISPLAY_ROTATION = 1;

// SD uses the higher allowed GPIO range so the TFT parallel bus stays on fast low GPIOs.
static const int SD_SCK_PIN = 40;
static const int SD_MISO_PIN = 39;
static const int SD_MOSI_PIN = 41;
static const int SD_CS_PIN = 42;

// XPT2046 touch shares the SD SPI clock/data lines and uses its own chip-select.
static const int TOUCH_SCK_PIN = SD_SCK_PIN;
static const int TOUCH_MISO_PIN = SD_MISO_PIN;
static const int TOUCH_MOSI_PIN = SD_MOSI_PIN;
static const int TOUCH_CS_PIN = 38;
static const int TOUCH_IRQ_PIN = TouchController::NoIrqPin;

// Optional card-detect pin. Leave as -1 when the socket/module has no CD pin.
static const int SD_CARD_DETECT_PIN = -1;

DisplayController *display = nullptr;
SdCardController *sdCard = nullptr;
TouchController *touch = nullptr;
Navigator *navigator = nullptr;
HomeScreen *homeScreen = nullptr;

void updateTouch();

void updateTouch() {
  if (!touch || !touch->update() || !navigator) {
    return;
  }

  Screen *screen = navigator->activeScreen();
  if (screen) {
    screen->handleTouch(touch->point());
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(500);

  Serial.println(F("ESP Media starting..."));

  display = new DisplayController();
  display->begin(DISPLAY_ROTATION);
  Serial.print(F("TFT size: "));
  Serial.print(display->width());
  Serial.print(F("x"));
  Serial.println(display->height());

  touch = new TouchController(
    TOUCH_SCK_PIN,
    TOUCH_MISO_PIN,
    TOUCH_MOSI_PIN,
    TOUCH_CS_PIN,
    TOUCH_IRQ_PIN
  );
  touch->begin(display->width(), display->height(), DISPLAY_ROTATION);
  Serial.println(F("XPT2046 touch ready."));

  sdCard = new SdCardController(
    SD_SCK_PIN,
    SD_MISO_PIN,
    SD_MOSI_PIN,
    SD_CS_PIN,
    SD_CARD_DETECT_PIN
  );

  if (sdCard->begin()) {
    Serial.println(F("SD card mounted."));
  } else {
    Serial.println(F("SD card not mounted."));
  }

  navigator = new Navigator();
  homeScreen = new HomeScreen(*navigator, *touch, *display, *sdCard);

  if (!navigator->registerScreen(ScreenId::Home, *homeScreen)) {
    Serial.println(F("Navigator failed to register Home screen."));
  }

  if (!navigator->setRoot(ScreenId::Home)) {
    Serial.println(F("Navigator failed to activate Home screen."));
  }
}

void loop() {
  updateTouch();

  if (navigator) {
    Screen *screen = navigator->activeScreen();
    if (screen) {
      screen->tick(millis());
    }
  }
}
