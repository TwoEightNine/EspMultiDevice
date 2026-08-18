#include <Arduino.h>

#include "DisplayController.h"
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

static const uint8_t TOUCH_LOG_LINES = 5;
static const int16_t TOUCH_LOG_LINE_HEIGHT = 18;

DisplayController *display = nullptr;
SdCardController *sdCard = nullptr;
TouchController *touch = nullptr;
String touchLog[TOUCH_LOG_LINES];
uint8_t touchLogCount = 0;
uint8_t touchLogNext = 0;
uint32_t touchEventCount = 0;

void redrawTouchLog();
void logTouchToDisplay(const String &message);
void updateTouch();
void runSdSelfTest();

void runSdSelfTest() {
  if (!sdCard || !sdCard->isMounted()) {
    return;
  }

  if (sdCard->runReadWriteTest()) {
    Serial.println(F("SD read/write test passed."));
    if (display) {
      display->showSdState(true, F("Read/write test passed"));
      redrawTouchLog();
    }
  } else {
    Serial.println(F("SD read/write test failed."));
    if (display) {
      display->showSdState(false, F("Read/write test failed"));
      redrawTouchLog();
    }
  }
}

void redrawTouchLog() {
  if (!display || !display->isReady()) {
    return;
  }

  TFT_eSPI &tft = display->tft();
  int16_t width = display->width();
  int16_t height = display->height();
  int16_t logHeight = TOUCH_LOG_LINE_HEIGHT * (TOUCH_LOG_LINES + 1) + 8;
  int16_t top = height - logHeight;

  tft.fillRect(0, top, width, logHeight, TFT_BLACK);
  tft.drawFastHLine(8, top, width - 16, TFT_DARKGREY);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(F("Touch log"), 10, top + 5, 2);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  for (uint8_t i = 0; i < touchLogCount; i++) {
    uint8_t index = (touchLogNext + TOUCH_LOG_LINES - touchLogCount + i) % TOUCH_LOG_LINES;
    tft.drawString(touchLog[index], 10, top + 25 + i * TOUCH_LOG_LINE_HEIGHT, 2);
  }
}

void logTouchToDisplay(const String &message) {
  touchLog[touchLogNext] = message;
  touchLogNext = (touchLogNext + 1) % TOUCH_LOG_LINES;

  if (touchLogCount < TOUCH_LOG_LINES) {
    touchLogCount++;
  }

  redrawTouchLog();
}

void updateTouch() {
  if (!touch || !touch->update()) {
    return;
  }

  TouchPoint point = touch->point();
  touchEventCount++;

  if (!point.touched) {
    Serial.println(F("Touch released."));
    String message = String(touchEventCount);
    message += F(": released");
    logTouchToDisplay(message);
    return;
  }

  Serial.print(F("Touch x="));
  Serial.print(point.x);
  Serial.print(F(" y="));
  Serial.print(point.y);
  Serial.print(F(" z="));
  Serial.print(point.z);
  Serial.print(F(" rawX="));
  Serial.print(point.rawX);
  Serial.print(F(" rawY="));
  Serial.println(point.rawY);

  if (display && display->isReady()) {
    display->tft().fillCircle(point.x, point.y, 4, TFT_YELLOW);
  }

  String message = String(touchEventCount);
  message += F(": x=");
  message += point.x;
  message += F(" y=");
  message += point.y;
  message += F(" z=");
  message += point.z;
  message += F(" raw=");
  message += point.rawX;
  message += F(",");
  message += point.rawY;
  logTouchToDisplay(message);
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

  display->showStatus(F("ESP Media"), F("Touch ready, mounting SD card..."));

  sdCard = new SdCardController(
    SD_SCK_PIN,
    SD_MISO_PIN,
    SD_MOSI_PIN,
    SD_CS_PIN,
    SD_CARD_DETECT_PIN
  );

  if (sdCard->begin()) {
    Serial.println(F("SD card mounted."));
    display->showSdState(true, F("Mounted"));
    redrawTouchLog();
    runSdSelfTest();
  } else {
    Serial.println(F("SD card not mounted."));
    display->showSdState(false, F("Waiting for card"));
    redrawTouchLog();
  }
}

void loop() {
  updateTouch();

  if (!sdCard) {
    return;
  }

  SdCardEvent event = sdCard->update();

  if (event == SdCardEvent::Mounted) {
    Serial.println(F("SD card inserted and mounted."));
    display->showSdState(true, F("Inserted and mounted"));
    redrawTouchLog();
    runSdSelfTest();
  } else if (event == SdCardEvent::Removed) {
    Serial.println(F("SD card removed or became unavailable."));
    display->showSdState(false, F("Removed or unavailable"));
    redrawTouchLog();
  } else if (event == SdCardEvent::MountFailed) {
    display->showSdState(false, F("Mount retry failed"));
    redrawTouchLog();
  }
}
