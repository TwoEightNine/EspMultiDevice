#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>
#include <hal/usb_serial_jtag_ll.h>
#include <sdkconfig.h>
#include <soc/soc_caps.h>

#if !SOC_USB_OTG_SUPPORTED
#error ESP Media USB Drive requires a native USB-capable ESP32 target
#endif
#if !defined(ARDUINO_USB_MODE) || ARDUINO_USB_MODE != 0
#error Select USB-OTG (TinyUSB) for USB Mode
#endif
#if ARDUINO_USB_CDC_ON_BOOT || ARDUINO_USB_MSC_ON_BOOT || ARDUINO_USB_DFU_ON_BOOT
#error Disable USB CDC, Firmware MSC, and DFU On Boot; ESP Media starts native USB explicitly
#endif
#if !defined(CONFIG_TINYUSB_MSC_ENABLED) || !CONFIG_TINYUSB_MSC_ENABLED
#error TinyUSB MSC support must be enabled for ESP Media USB Drive
#endif
#if !defined(CONFIG_TINYUSB_HID_ENABLED) || !CONFIG_TINYUSB_HID_ENABLED
#error TinyUSB HID support must be enabled for ESP Media Mac Remote
#endif

// TinyUSB collects every constructed interface when USB.begin() runs. Create
// MSC only in transfer boot so normal Mac Remote sessions remain HID-only.
USBMSC *usbMsc = nullptr;

#if defined(ESP_MEDIA_ASSISTANT_DEPENDENCY_PROBE) && defined(ESP_MEDIA_XIAOZHI_COMPATIBILITY_PROBE)
#error "Select only one assistant diagnostic"
#endif
#if defined(ESP_MEDIA_ASSISTANT_CONTRACT) && (defined(ESP_MEDIA_ASSISTANT_DEPENDENCY_PROBE) || defined(ESP_MEDIA_XIAOZHI_COMPATIBILITY_PROBE))
#error "Select only one assistant diagnostic"
#endif

#include <esp_system.h>

#include "src/core/BatteryController.h"
#include "src/services/storage/BatteryHistoryService.h"
#include "src/core/DisplayController.h"
#include "src/core/DisplayOrientation.h"
#include "src/services/audio/AudioSettings.h"
#include "src/services/led/LedSettings.h"
#include "src/services/assistant/AssistantBackendConfigLoader.h"
#include "src/services/assistant/AssistantController.h"
#include "src/features/assistant/AssistantScreen.h"
#include "src/features/settings/AudioSettingsScreen.h"
#include "src/features/settings/BatteryInfoScreen.h"
#include "src/features/settings/LedScreen.h"
#include "src/features/audio_player/AudioPlayerScreen.h"
#include "src/features/video_player/VideoPlayerScreen.h"
#include "src/features/text_viewer/TextViewerScreen.h"
#include "src/features/browser/BrowserScreen.h"
#include "src/features/files/FilesScreen.h"
#include "src/features/apps/AppHostScreen.h"
#include "src/features/home/HomeScreen.h"
#include "src/features/image_viewer/ImageViewerScreen.h"
#include "src/features/mac_remote/MacRemoteScreen.h"
#include "src/services/audio/MicController.h"
#include "src/features/splash/SplashScreen.h"
#include "src/app/Navigator.h"
#include "src/services/storage/SdCardController.h"
#include "src/features/settings/SettingsScreen.h"
#include "src/services/audio/SpeakerController.h"
#include "src/services/storage/SecretStorage.h"
#include "src/core/SpiBusLock.h"
#include "src/services/network/TimeController.h"
#include "src/core/TouchController.h"
#include "src/features/settings/TouchCalibrationScreen.h"
#include "src/features/settings/UsbDriveScreen.h"
#include "src/features/text_input/TextInputScreen.h"
#include "src/features/recording/VoiceRecorderScreen.h"
#include "src/services/network/WiFiController.h"
#include "src/services/storage/UsbDriveBoot.h"
#include "src/services/storage/UsbMassStorageController.h"
#include "src/services/usb/MacRemoteController.h"
#include "src/features/settings/WiFiScreen.h"
#include "src/features/settings/WiFiSpeedTestScreen.h"
#include "src/features/workout/WorkoutScreen.h"
#include "src/media/MediaCache.h"

#if defined(ESP_MEDIA_ASSISTANT_DEPENDENCY_PROBE)
#include "tests/assistant_dependency_probe/assistant_dependency_probe.ino"
#endif
#if defined(ESP_MEDIA_XIAOZHI_COMPATIBILITY_PROBE)
#include "tests/xiaozhi_compatibility_probe/xiaozhi_compatibility_probe.ino"
#endif
#if defined(ESP_MEDIA_ASSISTANT_CONTRACT)
#include "tests/assistant_contract/assistant_contract.ino"
#endif

static const uint32_t SERIAL_BAUD = 115200;
// Development-only diagnostic. Enable for three cold boots with a target card,
// then restore false before normal use.
static const bool RUN_SD_THROUGHPUT_BENCHMARK = false;
#if defined(ESP_MEDIA_ASSISTANT_DEPENDENCY_PROBE)
// Volatile keeps the complete production image linked into the diagnostic size
// measurement while selecting the isolated probe at runtime.
static volatile bool RUN_ASSISTANT_DEPENDENCY_PROBE = true;
#endif
#if defined(ESP_MEDIA_XIAOZHI_COMPATIBILITY_PROBE)
// Keep production composition linked while selecting the isolated compatibility probe.
static volatile bool RUN_XIAOZHI_COMPATIBILITY_PROBE = true;
#endif
#if defined(ESP_MEDIA_ASSISTANT_CONTRACT)
static volatile bool RUN_ASSISTANT_CONTRACT = true;
#endif

// Battery-divider midpoint. A 100 kΩ / 100 kΩ divider halves the cell voltage.
static const int BATTERY_ADC_PIN = 1;

// Keep the shared SD/touch SPI lines sequential beside the TFT parallel wiring.
static const int SD_SCK_PIN = 15;
static const int SD_MISO_PIN = 16;
static const int SD_MOSI_PIN = 17;
static const int SD_CS_PIN = 47;

// XPT2046 touch shares the SD SPI clock/data lines and uses its own chip-select.
static const int TOUCH_SCK_PIN = SD_SCK_PIN;
static const int TOUCH_MISO_PIN = SD_MISO_PIN;
static const int TOUCH_MOSI_PIN = SD_MOSI_PIN;
static const int TOUCH_CS_PIN = 21;
static const int TOUCH_IRQ_PIN = TouchController::NoIrqPin;

// Optional card-detect pin. Leave as -1 when the socket/module has no CD pin.
static const int SD_CARD_DETECT_PIN = -1;

// INMP441 I2S microphone. Power sense is optional but recommended with the physical mic switch.
static const int MIC_BCLK_PIN = 41;
static const int MIC_WS_PIN = 40;
static const int MIC_DATA_PIN = 39;
static const int MIC_POWER_SENSE_PIN = 38;

// Initial ESP32-S3-DevKitC-1 boards place their WS2812-compatible RGB LED on
// GPIO 48. DevKitC-1 v1.1 instead uses GPIO 38, which this project uses for
// microphone power sensing and therefore does not support as an LED GPIO.
static const int BUILTIN_RGB_LED_PIN = 48;

// MAX98357A I2S speaker amplifier.
static const int SPEAKER_BCLK_PIN = MIC_BCLK_PIN;
static const int SPEAKER_WS_PIN = MIC_WS_PIN;
static const int SPEAKER_DATA_PIN = 42;
static const int SPEAKER_ENABLE_PIN = 2;

DisplayController *display = nullptr;
SdCardController *sdCard = nullptr;
TouchController *touch = nullptr;
MicController *mic = nullptr;
SpeakerController *speaker = nullptr;
SecretStorage *secretStorage = nullptr;
MediaCache *mediaCache = nullptr;
AudioSettings *audioSettings = nullptr;
LedSettings *ledSettings = nullptr;
WiFiController *wifiController = nullptr;
TimeController *timeController = nullptr;
BatteryController *batteryController = nullptr;
BatteryHistoryService *batteryHistory = nullptr;
AssistantController *assistantController = nullptr;
UsbMassStorageController *usbMassStorage = nullptr;
MacRemoteController *macRemoteController = nullptr;

Navigator *navigator = nullptr;
HomeScreen *homeScreen = nullptr;
SplashScreen *splashScreen = nullptr;
VoiceRecorderScreen *voiceRecorderScreen = nullptr;
FilesScreen *filesScreen = nullptr;
SettingsScreen *settingsScreen = nullptr;
AudioSettingsScreen *audioSettingsScreen = nullptr;
LedScreen *ledScreen = nullptr;
BatteryInfoScreen *batteryInfoScreen = nullptr;
TouchCalibrationScreen *touchCalibrationScreen = nullptr;
ImageViewerScreen *imageViewerScreen = nullptr;
AudioPlayerScreen *audioPlayerScreen = nullptr;
VideoPlayerScreen *videoPlayerScreen = nullptr;
TextViewerScreen *textViewerScreen = nullptr;
TextInputScreen *textInputScreen = nullptr;
WiFiScreen *wifiScreen = nullptr;
WiFiSpeedTestScreen *wifiSpeedTestScreen = nullptr;
BrowserScreen *browserScreen = nullptr;
WorkoutScreen *workoutScreen = nullptr;
UsbDriveScreen *usbDriveScreen = nullptr;
MacRemoteScreen *macRemoteScreen = nullptr;
AssistantScreen *assistantScreen = nullptr;
AppHostScreen *appHostScreen = nullptr;

void updateTouch();

namespace {

const __FlashStringHelper *resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return F("power-on reset");
    case ESP_RST_EXT: return F("external reset pin");
    case ESP_RST_SW: return F("software reset");
    case ESP_RST_PANIC: return F("exception/panic reset");
    case ESP_RST_INT_WDT: return F("interrupt watchdog reset");
    case ESP_RST_TASK_WDT: return F("task watchdog reset");
    case ESP_RST_WDT: return F("other watchdog reset");
    case ESP_RST_DEEPSLEEP: return F("deep-sleep wake");
    case ESP_RST_BROWNOUT: return F("brownout reset (supply voltage sag)");
    case ESP_RST_SDIO: return F("SDIO reset");
    default: return F("unknown reset");
  }
}

void logResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.print(F("Reset cause: "));
  Serial.print(resetReasonName(reason));
  Serial.print(F(" ("));
  Serial.print(static_cast<int>(reason));
  Serial.println(F(")"));
}

} // namespace

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
#if defined(ESP_MEDIA_ASSISTANT_CONTRACT)
  if (RUN_ASSISTANT_CONTRACT) {
    assistantContractSetup();
    return;
  }
#endif
#if defined(ESP_MEDIA_ASSISTANT_DEPENDENCY_PROBE)
  if (RUN_ASSISTANT_DEPENDENCY_PROBE) {
    assistantDependencyProbeSetup();
    return;
  }
#endif
#if defined(ESP_MEDIA_XIAOZHI_COMPATIBILITY_PROBE)
  if (RUN_XIAOZHI_COMPATIBILITY_PROBE) {
    xiaozhiCompatibilityProbeSetup();
    return;
  }
#endif

  Serial.begin(SERIAL_BAUD);

  const bool usbTransferBoot = UsbDriveBoot::consumeRequest();
  if (!usbTransferBoot) {
    // The S3 ROM can leave its built-in USB-Serial/JTAG device attached even
    // when Arduino's USB-on-boot options are disabled. Disconnect that PHY in
    // normal operation so the native connector is reserved for USB Drive.
    usb_serial_jtag_ll_phy_enable_pad(false);
  }

  Serial.println(F("ESP Media starting..."));
  logResetReason();
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
  display->begin(DisplayOrientation::Portrait);
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
  touch->begin(display->width(), display->height(), DisplayOrientation::Portrait);
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
    if (RUN_SD_THROUGHPUT_BENCHMARK) {
      sdCard->runThroughputBenchmark();
    }
    if (touch->loadCalibration()) {
      Serial.println(F("Touch mesh calibration loaded."));
    }
  } else {
    Serial.println(F("SD card not mounted."));
  }

  navigator = new Navigator();
  if (usbTransferBoot) {
    usbMsc = new USBMSC();
    usbMassStorage = new UsbMassStorageController(*sdCard, *usbMsc);
    usbDriveScreen = new UsbDriveScreen(
      *navigator, *touch, *display, usbMassStorage, true
    );
    if (!navigator->registerScreen(ScreenId::UsbDrive, *usbDriveScreen) ||
        !navigator->setRoot(ScreenId::UsbDrive)) {
      Serial.println(F("USB Drive screen setup failed."));
    } else {
      Serial.println(F("USB Drive transfer boot ready."));
    }
    return;
  }

  batteryController = new BatteryController(BATTERY_ADC_PIN);
  batteryController->begin();
  Serial.println(F("Battery controller ready."));
  batteryHistory = new BatteryHistoryService(*batteryController, *sdCard);
  Serial.println(F("Battery history ready; first sample deferred until splash exit."));

  mic = new MicController(
    MIC_BCLK_PIN,
    MIC_WS_PIN,
    MIC_DATA_PIN,
    MIC_POWER_SENSE_PIN
  );
  if (mic->begin()) {
    Serial.println(F("INMP441 microphone ready; power-switch sensing enabled."));
  } else {
    Serial.print(F("INMP441 microphone failed: "));
    Serial.println(mic->lastError());
  }

  speaker = new SpeakerController(
    SPEAKER_BCLK_PIN,
    SPEAKER_WS_PIN,
    SPEAKER_DATA_PIN,
    SPEAKER_ENABLE_PIN
  );
  speaker->beginControl();
  Serial.println(F("MAX98357A speaker muted; I2S configured on playback."));

  secretStorage = new SecretStorage();
  mediaCache = new MediaCache(*secretStorage);

  audioSettings = new AudioSettings(*speaker);
  audioSettings->begin();
  Serial.print(F("Audio volume: "));
  Serial.println(audioSettings->volume());

  ledSettings = new LedSettings(BUILTIN_RGB_LED_PIN);
  if (ledSettings->begin()) {
    Serial.println(F("Built-in RGB LED ready."));
  } else {
    Serial.print(F("Built-in RGB LED unavailable: "));
    Serial.println(ledSettings->error());
  }

  wifiController = new WiFiController();
  wifiController->begin();
  Serial.println(F("Wi-Fi controller ready."));

  timeController = new TimeController(*wifiController);
  timeController->begin();
  Serial.println(F("Time controller ready."));

  // Construct HID descriptors only in normal composition. The controller
  // starts the native peripheral lazily when Mac Remote is first opened.
  macRemoteController = new MacRemoteController();

  const AssistantBackendConfig assistantBackend = loadAssistantBackendConfig();
  assistantController = new AssistantController(
    *wifiController, *timeController, *mic, *speaker, *sdCard,
    *batteryHistory, *audioSettings, *ledSettings, assistantBackend
  );
  if (!assistantController->begin()) {
    Serial.println(F("Assistant worker unavailable."));
  }

  homeScreen = new HomeScreen(*navigator, *touch, *display, *sdCard, *wifiController,
                              *timeController, *batteryHistory);
  splashScreen = new SplashScreen(*navigator, *touch, *display, *wifiController,
                                  *batteryHistory);
  voiceRecorderScreen = new VoiceRecorderScreen(*navigator, *touch, *display, *sdCard, *mic, *speaker);
  filesScreen = new FilesScreen(*navigator, *touch, *display, *sdCard, *secretStorage, *mediaCache);
  settingsScreen = new SettingsScreen(*navigator, *touch, *display);
  audioSettingsScreen = new AudioSettingsScreen(*navigator, *touch, *display, *audioSettings);
  ledScreen = new LedScreen(*navigator, *touch, *display, *ledSettings);
  batteryInfoScreen = new BatteryInfoScreen(*navigator, *touch, *display, *batteryHistory);
  touchCalibrationScreen = new TouchCalibrationScreen(*navigator, *touch, *display);
  imageViewerScreen = new ImageViewerScreen(*navigator, *touch, *display, *sdCard, *secretStorage, *mediaCache);
  audioPlayerScreen = new AudioPlayerScreen(*navigator, *touch, *display, *sdCard, *speaker, *audioSettings, *secretStorage, *mediaCache);
  videoPlayerScreen = new VideoPlayerScreen(*navigator, *touch, *display, *sdCard, *speaker, *secretStorage);
  textViewerScreen = new TextViewerScreen(*navigator, *touch, *display, *sdCard, *secretStorage);
  textInputScreen = new TextInputScreen(*navigator, *touch, *display);
  wifiScreen = new WiFiScreen(*navigator, *touch, *display, *wifiController);
  wifiSpeedTestScreen = new WiFiSpeedTestScreen(*navigator, *touch, *display,
                                                *wifiController, *sdCard);
  browserScreen = new BrowserScreen(*navigator, *touch, *display, *wifiController, *sdCard);
  workoutScreen = new WorkoutScreen(*navigator, *touch, *display, *speaker, *audioSettings);
  usbDriveScreen = new UsbDriveScreen(*navigator, *touch, *display, nullptr, false);
  macRemoteScreen = new MacRemoteScreen(*navigator, *touch, *display,
                                        *macRemoteController);
  assistantScreen = new AssistantScreen(
    *navigator, *touch, *display, *assistantController, *sdCard
  );
  appHostScreen = new AppHostScreen(*navigator, *touch, *display, *sdCard,
                                    *wifiController, *batteryHistory,
                                    *speaker, *audioSettings);

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
  if (!navigator->registerScreen(ScreenId::Led, *ledScreen)) {
    Serial.println(F("Navigator failed to register LED screen."));
  }
  if (!navigator->registerScreen(ScreenId::BatteryInfo, *batteryInfoScreen)) {
    Serial.println(F("Navigator failed to register Battery Info screen."));
  }
  if (!navigator->registerScreen(ScreenId::WiFi, *wifiScreen)) {
    Serial.println(F("Navigator failed to register Wi-Fi screen."));
  }
  if (!navigator->registerScreen(ScreenId::WiFiSpeedTest, *wifiSpeedTestScreen)) {
    Serial.println(F("Navigator failed to register Wi-Fi Speed Test screen."));
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
  if (!navigator->registerScreen(ScreenId::TextViewer, *textViewerScreen)) {
    Serial.println(F("Navigator failed to register Text Viewer screen."));
  }
  if (!navigator->registerScreen(ScreenId::TextInput, *textInputScreen)) {
    Serial.println(F("Navigator failed to register Text Input screen."));
  }
  if (!navigator->registerScreen(ScreenId::Browser, *browserScreen)) {
    Serial.println(F("Navigator failed to register Browser screen."));
  }
  if (!navigator->registerScreen(ScreenId::Workout, *workoutScreen)) {
    Serial.println(F("Navigator failed to register Workout screen."));
  }
  if (!navigator->registerScreen(ScreenId::Assistant, *assistantScreen)) {
    Serial.println(F("Navigator failed to register Assistant screen."));
  }
  if (!navigator->registerScreen(ScreenId::UsbDrive, *usbDriveScreen)) {
    Serial.println(F("Navigator failed to register USB Drive screen."));
  }
  if (!navigator->registerScreen(ScreenId::MacRemote, *macRemoteScreen)) {
    Serial.println(F("Navigator failed to register Mac Remote screen."));
  }
  if (!navigator->registerScreen(ScreenId::ExternalApp, *appHostScreen)) {
    Serial.println(F("Navigator failed to register external app screen."));
  }

  if (!navigator->setRoot(ScreenId::Splash)) {
    Serial.println(F("Navigator failed to activate Splash screen."));
  }

  Serial.println(F("ESP Media setup complete."));
}

void loop() {
#if defined(ESP_MEDIA_ASSISTANT_CONTRACT)
  if (RUN_ASSISTANT_CONTRACT) {
    assistantContractLoop();
    return;
  }
#endif
#if defined(ESP_MEDIA_ASSISTANT_DEPENDENCY_PROBE)
  if (RUN_ASSISTANT_DEPENDENCY_PROBE) {
    assistantDependencyProbeLoop();
    return;
  }
#endif
#if defined(ESP_MEDIA_XIAOZHI_COMPATIBILITY_PROBE)
  if (RUN_XIAOZHI_COMPATIBILITY_PROBE) {
    xiaozhiCompatibilityProbeLoop();
    return;
  }
#endif

  updateTouch();

  // Keep hot-swap state current even while a screen that does not directly
  // use the SD controller (for example Settings or the browser) is active.
  if (sdCard) {
    sdCard->update();
    if (!sdCard->isMounted() && secretStorage) secretStorage->lock();
  }

  if (batteryHistory) {
    batteryHistory->update(millis());
  }

  if (wifiController) {
    wifiController->update(millis());
  }

  if (timeController) {
    timeController->update(millis());
  }

  if (navigator) {
    navigator->tick(millis());
  }
}
