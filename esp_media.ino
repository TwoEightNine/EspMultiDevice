#include <Arduino.h>

#include "DisplayController.h"
#include "FilesScreen.h"
#include "HomeScreen.h"
#include "ImageViewerScreen.h"
#include "MicController.h"
#include "SplashScreen.h"
#include "Navigator.h"
#include "SdCardController.h"
#include "SettingsScreen.h"
#include "SpeakerController.h"
#include "TouchController.h"
#include "VoiceRecorderScreen.h"

static const uint32_t SERIAL_BAUD = 115200;
static const uint8_t DISPLAY_ROTATION = 4;

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
static const uint16_t TOUCH_PRESSURE_THRESHOLD = 1100;

// Optional card-detect pin. Leave as -1 when the socket/module has no CD pin.
static const int SD_CARD_DETECT_PIN = -1;

// INMP441 I2S microphone. Power sense is optional but recommended with the physical mic switch.
static const int MIC_BCLK_PIN = 18;
static const int MIC_WS_PIN = 21;
static const int MIC_DATA_PIN = 17;
static const int MIC_POWER_SENSE_PIN = 48;

// MAX98357A I2S speaker amplifier.
static const int SPEAKER_BCLK_PIN = MIC_BCLK_PIN;
static const int SPEAKER_WS_PIN = MIC_WS_PIN;
static const int SPEAKER_DATA_PIN = 1;
static const int SPEAKER_ENABLE_PIN = 2;

DisplayController *display = nullptr;
SdCardController *sdCard = nullptr;
TouchController *touch = nullptr;
MicController *mic = nullptr;
SpeakerController *speaker = nullptr;

Navigator *navigator = nullptr;
HomeScreen *homeScreen = nullptr;
SplashScreen *splashScreen = nullptr;
VoiceRecorderScreen *voiceRecorderScreen = nullptr;
FilesScreen *filesScreen = nullptr;
SettingsScreen *settingsScreen = nullptr;
ImageViewerScreen *imageViewerScreen = nullptr;

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
  touch->setPressureThreshold(TOUCH_PRESSURE_THRESHOLD);
  touch->begin(display->width(), display->height(), DISPLAY_ROTATION);
  Serial.print(F("XPT2046 touch ready. Pressure threshold: "));
  Serial.println(touch->pressureThreshold());

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

  mic = new MicController(
    MIC_BCLK_PIN,
    MIC_WS_PIN,
    MIC_DATA_PIN,
    MIC_POWER_SENSE_PIN
  );
  Serial.println(F("INMP441 microphone configured for lazy startup."));

  speaker = new SpeakerController(
    SPEAKER_BCLK_PIN,
    SPEAKER_WS_PIN,
    SPEAKER_DATA_PIN,
    SPEAKER_ENABLE_PIN
  );
  Serial.println(F("MAX98357A speaker configured for lazy startup."));

  navigator = new Navigator();
  homeScreen = new HomeScreen(*navigator, *touch, *display, *sdCard);
  splashScreen = new SplashScreen(*navigator, *touch, *display);
  voiceRecorderScreen = new VoiceRecorderScreen(*navigator, *touch, *display, *sdCard, *mic, *speaker);
  filesScreen = new FilesScreen(*navigator, *touch, *display, *sdCard);
  settingsScreen = new SettingsScreen(*navigator, *touch, *display);
  imageViewerScreen = new ImageViewerScreen(*navigator, *touch, *display, *sdCard);

  if (!navigator->registerScreen(ScreenId::Splash, *splashScreen)) {
    Serial.println(F("Navigator failed to register Splash screen."));
  }
  if (!navigator->registerScreen(ScreenId::Home, *homeScreen)) {
    Serial.println(F("Navigator failed to register Home screen."));
  }
  if (!navigator->registerScreen(ScreenId::VoiceRecorder, *voiceRecorderScreen)) {
    Serial.println(F("Navigator failed to register Voice Recorder screen."));
  }
  if (!navigator->registerScreen(ScreenId::Files, *filesScreen)) {
    Serial.println(F("Navigator failed to register Files screen."));
  }
  if (!navigator->registerScreen(ScreenId::Settings, *settingsScreen)) {
    Serial.println(F("Navigator failed to register Settings screen."));
  }
  if (!navigator->registerScreen(ScreenId::ImageViewer, *imageViewerScreen)) {
    Serial.println(F("Navigator failed to register Image Viewer screen."));
  }

  if (!navigator->setRoot(ScreenId::Splash)) {
    Serial.println(F("Navigator failed to activate Splash screen."));
  }

  Serial.println(F("ESP Media setup complete."));
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
