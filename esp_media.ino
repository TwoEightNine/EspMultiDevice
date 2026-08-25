#include <Arduino.h>

#include "DisplayController.h"
#include "AudioSettings.h"
#include "AudioSettingsScreen.h"
#include "AudioPlayerScreen.h"
#include "VideoPlayerScreen.h"
#include "BrowserScreen.h"
#include "FilesScreen.h"
#include "HomeScreen.h"
#include "ImageViewerScreen.h"
#include "MicController.h"
#include "SplashScreen.h"
#include "Navigator.h"
#include "SdCardController.h"
#include "SettingsScreen.h"
#include "SpeakerController.h"
#include "SpiBusLock.h"
#include "TimeController.h"
#include "TouchController.h"
#include "TouchCalibrationScreen.h"
#include "TextInputScreen.h"
#include "VoiceRecorderScreen.h"
#include "WiFiController.h"
#include "WiFiScreen.h"

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
AudioSettings *audioSettings = nullptr;
WiFiController *wifiController = nullptr;
TimeController *timeController = nullptr;

Navigator *navigator = nullptr;
HomeScreen *homeScreen = nullptr;
SplashScreen *splashScreen = nullptr;
VoiceRecorderScreen *voiceRecorderScreen = nullptr;
FilesScreen *filesScreen = nullptr;
SettingsScreen *settingsScreen = nullptr;
AudioSettingsScreen *audioSettingsScreen = nullptr;
TouchCalibrationScreen *touchCalibrationScreen = nullptr;
ImageViewerScreen *imageViewerScreen = nullptr;
AudioPlayerScreen *audioPlayerScreen = nullptr;
VideoPlayerScreen *videoPlayerScreen = nullptr;
TextInputScreen *textInputScreen = nullptr;
WiFiScreen *wifiScreen = nullptr;
BrowserScreen *browserScreen = nullptr;

void updateTouch();

void updateTouch() {
  // TouchController::update() also reports coordinate changes while a finger
  // remains on the panel.  Deliver only state edges to screens so a screen
  // opened by a press cannot receive that same held press as a new click.
  static bool touchWasDown = false;

  if (!touch || !touch->update()) {
    return;
  }

  TouchPoint point = touch->point();
  if (point.touched == touchWasDown) {
    return;
  }
  touchWasDown = point.touched;

  if (!navigator) {
    return;
  }

  Screen *screen = navigator->activeScreen();
  if (screen) {
    screen->handleTouch(point);
  }
}

void setup() {
  Serial.begin(SERIAL_BAUD);

  Serial.println(F("ESP Media starting..."));
  Serial.print(F("CPU MHz: "));
  Serial.println(getCpuFrequencyMhz());
  Serial.print(F("Flash bytes: "));
  Serial.println(ESP.getFlashChipSize());
  Serial.print(F("PSRAM total/free/max: "));
  Serial.print(ESP.getPsramSize());
  Serial.print('/');
  Serial.print(ESP.getFreePsram());
  Serial.print('/');
  Serial.println(ESP.getMaxAllocPsram());
  if (ESP.getPsramSize() < 8UL * 1024UL * 1024UL) {
    Serial.println(F("Warning: GIF ring playback expects 8 MiB PSRAM (N16R8 build profile)."));
  }

  if (!SpiBusLock::begin()) {
    Serial.println(F("Shared SPI lock unavailable."));
  }

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
    if (touch->loadCalibration()) {
      Serial.println(F("Touch mesh calibration loaded."));
    }
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
  speaker->beginControl();
  Serial.println(F("MAX98357A speaker muted; I2S configured on playback."));

  audioSettings = new AudioSettings(*speaker);
  audioSettings->begin();
  Serial.print(F("Audio volume: "));
  Serial.println(audioSettings->volume());

  wifiController = new WiFiController();
  wifiController->begin();
  Serial.println(F("Wi-Fi controller ready."));

  timeController = new TimeController(*wifiController);
  timeController->begin();
  Serial.println(F("Time controller ready."));

  navigator = new Navigator();
  homeScreen = new HomeScreen(*navigator, *touch, *display, *sdCard, *wifiController, *timeController);
  splashScreen = new SplashScreen(*navigator, *touch, *display, *wifiController);
  voiceRecorderScreen = new VoiceRecorderScreen(*navigator, *touch, *display, *sdCard, *mic, *speaker);
  filesScreen = new FilesScreen(*navigator, *touch, *display, *sdCard);
  settingsScreen = new SettingsScreen(*navigator, *touch, *display);
  audioSettingsScreen = new AudioSettingsScreen(*navigator, *touch, *display, *audioSettings);
  touchCalibrationScreen = new TouchCalibrationScreen(*navigator, *touch, *display);
  imageViewerScreen = new ImageViewerScreen(*navigator, *touch, *display, *sdCard);
  audioPlayerScreen = new AudioPlayerScreen(*navigator, *touch, *display, *sdCard, *speaker, *audioSettings);
  videoPlayerScreen = new VideoPlayerScreen(*navigator, *touch, *display, *sdCard, *speaker);
  textInputScreen = new TextInputScreen(*navigator, *touch, *display);
  wifiScreen = new WiFiScreen(*navigator, *touch, *display, *wifiController);
  browserScreen = new BrowserScreen(*navigator, *touch, *display, *wifiController, *sdCard);

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
  if (!navigator->registerScreen(ScreenId::AudioSettings, *audioSettingsScreen)) {
    Serial.println(F("Navigator failed to register Audio Settings screen."));
  }
  if (!navigator->registerScreen(ScreenId::WiFi, *wifiScreen)) {
    Serial.println(F("Navigator failed to register Wi-Fi screen."));
  }
  if (!navigator->registerScreen(ScreenId::TouchCalibration, *touchCalibrationScreen)) {
    Serial.println(F("Navigator failed to register Touch Calibration screen."));
  }
  if (!navigator->registerScreen(ScreenId::ImageViewer, *imageViewerScreen)) {
    Serial.println(F("Navigator failed to register Image Viewer screen."));
  }
  if (!navigator->registerScreen(ScreenId::AudioPlayer, *audioPlayerScreen)) {
    Serial.println(F("Navigator failed to register Audio Player screen."));
  }
  if (!navigator->registerScreen(ScreenId::VideoPlayer, *videoPlayerScreen)) {
    Serial.println(F("Navigator failed to register Video Player screen."));
  }
  if (!navigator->registerScreen(ScreenId::TextInput, *textInputScreen)) {
    Serial.println(F("Navigator failed to register Text Input screen."));
  }
  if (!navigator->registerScreen(ScreenId::Browser, *browserScreen)) {
    Serial.println(F("Navigator failed to register Browser screen."));
  }

  if (!navigator->setRoot(ScreenId::Splash)) {
    Serial.println(F("Navigator failed to activate Splash screen."));
  }

  Serial.println(F("ESP Media setup complete."));
}

void loop() {
  updateTouch();

  if (wifiController) {
    wifiController->update(millis());
  }

  if (timeController) {
    timeController->update(millis());
  }

  if (navigator) {
    Screen *screen = navigator->activeScreen();
    if (screen) {
      screen->tick(millis());
    }
  }
}
