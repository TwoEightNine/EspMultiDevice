#include "UsbDriveScreen.h"

#include <ESP.h>

#include "../../app/Navigator.h"
#include "../../services/storage/UsbDriveBoot.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"

namespace {
const int16_t ButtonWidth = 220;
const int16_t ButtonHeight = 48;
const int16_t ButtonTop = 244;
const uint16_t ActionColor = 0x04df;
}

UsbDriveScreen::UsbDriveScreen(Navigator &navigator,
                               TouchController &touch,
                               DisplayController &display,
                               UsbMassStorageController *usbStorage,
                               bool transferBoot)
  : Screen(navigator, touch, display),
    _usbStorage(usbStorage),
    _transferBoot(transferBoot),
    _serviceStarted(false),
    _lastTouchDown(false),
    _restartPending(false),
    _restartAtMs(0),
    _lastState(UsbDriveState::WaitingForCard),
    _message(),
    _primaryButton{0, 0, 0, 0} {
}

void UsbDriveScreen::onEnter() {
  _lastTouchDown = false;
  _restartPending = false;
  _message = "";
  if (_transferBoot && _usbStorage && !_serviceStarted) {
    const bool usbStarted = _usbStorage->begin();
    _serviceStarted = true;
    _lastState = _usbStorage->state();
    if (!usbStarted) {
      Serial.println(F("Native USB failed to start; UART recovery remains available."));
    }
  }
  draw();
}

void UsbDriveScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown || _restartPending) return;
  _lastTouchDown = true;

  if (!_transferBoot) {
    if (UiKit::navigationIconContains(point.x, point.y)) {
      _navigator.pop();
    } else if (_primaryButton.contains(point.x, point.y)) {
      requestTransferBoot();
    }
    return;
  }

  const UsbDriveState current = _usbStorage
    ? _usbStorage->state() : UsbDriveState::IoError;
  if (UiKit::navigationIconContains(point.x, point.y)) {
    if (_usbStorage && _usbStorage->cancelBeforeHostAccess()) {
      _lastState = UsbDriveState::Cancelled;
      draw();
      scheduleRestart(millis());
    } else if (current == UsbDriveState::Active) {
      _message = F("Eject the SD volume on your Mac first");
      draw();
    }
    return;
  }

  if (_primaryButton.contains(point.x, point.y) &&
      (current == UsbDriveState::UsbInitError ||
       current == UsbDriveState::IoError)) {
    scheduleRestart(millis());
  }
}

void UsbDriveScreen::tick(uint32_t nowMs) {
  if (_restartPending) {
    if (static_cast<int32_t>(nowMs - _restartAtMs) >= 0) restartNow();
    return;
  }
  if (!_transferBoot || !_usbStorage) return;

  _usbStorage->tick();
  const UsbDriveState current = _usbStorage->state();
  if (current != _lastState) {
    _lastState = current;
    _message = "";
    draw();
  }
  if (current == UsbDriveState::Ejected ||
      current == UsbDriveState::Disconnected ||
      current == UsbDriveState::Cancelled) {
    scheduleRestart(nowMs);
  }
}

void UsbDriveScreen::draw() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("USB Drive"));
  _primaryButton = {0, 0, 0, 0};
  if (_transferBoot) drawTransferState(_lastState);
  else drawIntro();
}

void UsbDriveScreen::drawIntro() {
  TFT_eSPI &tft = _display.tft();
  UiText::draw(tft, F("Share the entire SD card with your Mac"),
               _display.width() / 2, 82, TFT_WHITE, TFT_BLACK,
               UiText::Size::Content, MC_DATUM);
  UiText::draw(tft, F("The device will restart in transfer mode."),
               _display.width() / 2, 122, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  UiText::draw(tft, F("When finished, eject the SD volume in Finder."),
               _display.width() / 2, 148, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);
  UiText::draw(tft, F("Do not unplug the cable while files are copying."),
               _display.width() / 2, 174, TFT_ORANGE, TFT_BLACK,
               UiText::Size::Compact, MC_DATUM);

  if (_message.length()) {
    UiText::draw(tft, _message, _display.width() / 2, 214,
                 TFT_RED, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
  }
  _primaryButton = {
    static_cast<int16_t>((_display.width() - ButtonWidth) / 2),
    ButtonTop, ButtonWidth, ButtonHeight
  };
  UiKit::drawFilledButton(tft, _primaryButton.x, _primaryButton.y,
                          _primaryButton.w, _primaryButton.h,
                          F("Start USB Drive"), ActionColor);
}

void UsbDriveScreen::drawTransferState(UsbDriveState state) {
  TFT_eSPI &tft = _display.tft();
  String title;
  String detail;
  uint16_t color = TFT_WHITE;
  switch (state) {
    case UsbDriveState::WaitingForCard:
      title = F("Insert an SD card");
      detail = F("The card will appear in Finder when ready.");
      color = TFT_ORANGE;
      break;
    case UsbDriveState::Ready:
      title = F("SD card ready");
      detail = F("Connect the native USB port to your Mac.");
      color = TFT_CYAN;
      break;
    case UsbDriveState::Active:
      title = F("Connected to Mac");
      detail = F("Transfer files, then eject the SD volume in Finder.");
      color = TFT_GREEN;
      break;
    case UsbDriveState::Ejected:
      title = F("Safely ejected");
      detail = F("Restarting in normal mode...");
      color = TFT_GREEN;
      break;
    case UsbDriveState::Disconnected:
      title = F("USB disconnected");
      detail = F("Restarting in normal mode...");
      color = TFT_ORANGE;
      break;
    case UsbDriveState::Cancelled:
      title = F("Transfer cancelled");
      detail = F("Restarting in normal mode...");
      color = TFT_LIGHTGREY;
      break;
    case UsbDriveState::UsbInitError:
      title = F("USB could not start");
      detail = F("Restart and check the USB build settings.");
      color = TFT_RED;
      break;
    case UsbDriveState::IoError:
      title = F("SD transfer error");
      detail = F("Restart the device and check the card.");
      color = TFT_RED;
      break;
  }

  UiText::draw(tft, title, _display.width() / 2, 104,
               color, TFT_BLACK, UiText::Size::Title, MC_DATUM);
  UiText::draw(tft, detail, _display.width() / 2, 150,
               TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
  if (state == UsbDriveState::Active) {
    UiText::draw(tft, F("The SD card is unavailable on this device"),
                 _display.width() / 2, 184, TFT_ORANGE, TFT_BLACK,
                 UiText::Size::Compact, MC_DATUM);
    UiText::draw(tft, F("until Finder ejects it."),
                 _display.width() / 2, 208, TFT_ORANGE, TFT_BLACK,
                 UiText::Size::Compact, MC_DATUM);
  }
  if (_message.length()) {
    UiText::draw(tft, _message, _display.width() / 2, 226,
                 TFT_ORANGE, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
  }
  if (state == UsbDriveState::UsbInitError ||
      state == UsbDriveState::IoError) {
    _primaryButton = {
      static_cast<int16_t>((_display.width() - ButtonWidth) / 2),
      ButtonTop, ButtonWidth, ButtonHeight
    };
    UiKit::drawFilledButton(tft, _primaryButton.x, _primaryButton.y,
                            _primaryButton.w, _primaryButton.h,
                            F("Restart"), TFT_DARKGREY);
  } else if (state == UsbDriveState::WaitingForCard ||
             state == UsbDriveState::Ready) {
    UiText::draw(tft, F("Tap X to cancel before the Mac connects"),
                 _display.width() / 2, 278, TFT_DARKGREY, TFT_BLACK,
                 UiText::Size::Compact, MC_DATUM);
  }
}

void UsbDriveScreen::requestTransferBoot() {
  if (!UsbDriveBoot::request()) {
    _message = F("Could not save USB Drive request");
    draw();
    return;
  }
  _message = F("Restarting in USB Drive mode...");
  draw();
  scheduleRestart(millis());
}

void UsbDriveScreen::scheduleRestart(uint32_t nowMs) {
  if (_restartPending) return;
  _restartPending = true;
  _restartAtMs = nowMs + RestartDelayMs;
}

void UsbDriveScreen::restartNow() {
  ESP.restart();
}
