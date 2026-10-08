#include "MacRemoteController.h"

#include <USB.h>
#include <hal/usb_serial_jtag_ll.h>

MacRemoteController *MacRemoteController::_instance = nullptr;

MacRemoteController::MacRemoteController()
  : _keyboard(),
    _consumerControl(),
    _stateMux(portMUX_INITIALIZER_UNLOCKED),
    _state(MacRemoteState::NotStarted),
    _started(false),
    _hasConnected(false),
    _macroStep(MacroStep::Idle),
    _macroAtMs(0),
    _macroIndex(0),
    _macroUrl{} {
}

bool MacRemoteController::begin() {
  portENTER_CRITICAL(&_stateMux);
  const bool alreadyStarted = _started;
  portEXIT_CRITICAL(&_stateMux);
  if (alreadyStarted) return state() != MacRemoteState::Error;

  _instance = this;
  USB.onEvent(onUsbEvent);
  USB.manufacturerName("ESPMedia");
  USB.productName("ESP Media Mac Remote");
  USB.firmwareVersion(0x0100);

  _keyboard.begin(KeyboardLayout_en_US);
  _consumerControl.begin();

  // Normal boot deliberately disconnects this pad. Reconnect it only when
  // the user opens Mac Remote so the native connector remains opt-in.
  usb_serial_jtag_ll_phy_enable_pad(true);
  if (!USB.begin()) {
    portENTER_CRITICAL(&_stateMux);
    _started = true;
    _state = MacRemoteState::Error;
    portEXIT_CRITICAL(&_stateMux);
    return false;
  }

  portENTER_CRITICAL(&_stateMux);
  _started = true;
  if (_state == MacRemoteState::NotStarted) {
    _state = MacRemoteState::WaitingForHost;
  }
  portEXIT_CRITICAL(&_stateMux);
  return true;
}

void MacRemoteController::tick(uint32_t nowMs) {
  if (_macroStep == MacroStep::Idle) return;
  if (state() != MacRemoteState::Connected) {
    releaseAll();
    return;
  }
  if (static_cast<int32_t>(nowMs - _macroAtMs) < 0) return;

  if (_macroStep == MacroStep::WaitForSpotlight) {
    _macroStep = MacroStep::TypeUrl;
    _macroAtMs = nowMs;
    return;
  }

  if (_macroStep == MacroStep::TypeUrl) {
    const char next = _macroUrl[_macroIndex];
    if (next) {
      if (_keyboard.write(static_cast<uint8_t>(next)) != 1) {
        releaseAll();
        return;
      }
      _keyboard.releaseAll();
      ++_macroIndex;
      _macroAtMs = nowMs + 8;
      return;
    }
    if (_macroIndex == 0) {
      cancelMacro();
      return;
    }
    _macroStep = MacroStep::WaitForResult;
    _macroAtMs = nowMs + 350;
    return;
  }

  if (_macroStep == MacroStep::WaitForResult) {
    if (_keyboard.write(KEY_DOWN_ARROW) != 1) {
      releaseAll();
      return;
    }
    _keyboard.releaseAll();
    _macroStep = MacroStep::WaitForSelection;
    _macroAtMs = nowMs + 100;
    return;
  }

  if (_macroStep == MacroStep::WaitForSelection) {
    _keyboard.write(KEY_RETURN);
    _keyboard.releaseAll();
    _macroStep = MacroStep::Idle;
    _macroAtMs = 0;
    _macroIndex = 0;
    _macroUrl[0] = '\0';
  }
}

MacRemoteState MacRemoteController::state() const {
  portENTER_CRITICAL(&_stateMux);
  const MacRemoteState snapshot = _state;
  portEXIT_CRITICAL(&_stateMux);
  return snapshot;
}

bool MacRemoteController::hasConnected() const {
  portENTER_CRITICAL(&_stateMux);
  const bool snapshot = _hasConnected;
  portEXIT_CRITICAL(&_stateMux);
  return snapshot;
}

bool MacRemoteController::busy() const {
  return _macroStep != MacroStep::Idle;
}

bool MacRemoteController::send(MacRemoteAction action) {
  if (state() != MacRemoteState::Connected || busy()) return false;

  switch (action) {
    case MacRemoteAction::YouTube:
      return startUrlMacro("https://www.youtube.com/", millis());
    case MacRemoteAction::Screenshot:
      return sendKeyboardShortcut(KEY_LEFT_SHIFT, KEY_LEFT_GUI, '3');
    case MacRemoteAction::Lock:
      return sendKeyboardShortcut(KEY_LEFT_CTRL, KEY_LEFT_GUI, 'q');
    case MacRemoteAction::Previous:
      return sendConsumerControl(CONSUMER_CONTROL_SCAN_PREVIOUS);
    case MacRemoteAction::PlayPause:
      return sendConsumerControl(CONSUMER_CONTROL_PLAY_PAUSE);
    case MacRemoteAction::Next:
      return sendConsumerControl(CONSUMER_CONTROL_SCAN_NEXT);
    case MacRemoteAction::Mute:
      return sendConsumerControl(CONSUMER_CONTROL_MUTE);
    case MacRemoteAction::VolumeDown:
      return sendConsumerControl(CONSUMER_CONTROL_VOLUME_DECREMENT);
    case MacRemoteAction::VolumeUp:
      return sendConsumerControl(CONSUMER_CONTROL_VOLUME_INCREMENT);
    default:
      return false;
  }
}

void MacRemoteController::releaseAll() {
  cancelMacro();
  portENTER_CRITICAL(&_stateMux);
  const bool started = _started;
  portEXIT_CRITICAL(&_stateMux);
  if (!started) return;

  _keyboard.releaseAll();
  _consumerControl.release();
}

void MacRemoteController::onUsbEvent(void *arg, esp_event_base_t eventBase,
                                     int32_t eventId, void *eventData) {
  (void)arg;
  (void)eventData;
  if (!_instance || eventBase != ARDUINO_USB_EVENTS) return;

  switch (eventId) {
    case ARDUINO_USB_STARTED_EVENT:
      portENTER_CRITICAL(&_instance->_stateMux);
      _instance->_state = MacRemoteState::Connected;
      _instance->_hasConnected = true;
      portEXIT_CRITICAL(&_instance->_stateMux);
      break;
    case ARDUINO_USB_STOPPED_EVENT:
      _instance->setState(MacRemoteState::WaitingForHost);
      break;
    case ARDUINO_USB_SUSPEND_EVENT:
      _instance->setState(MacRemoteState::Suspended);
      break;
    case ARDUINO_USB_RESUME_EVENT:
      _instance->setState(MacRemoteState::Connected);
      break;
    default:
      break;
  }
}

void MacRemoteController::setState(MacRemoteState state) {
  portENTER_CRITICAL(&_stateMux);
  _state = state;
  portEXIT_CRITICAL(&_stateMux);
}

bool MacRemoteController::startUrlMacro(const char *url, uint32_t nowMs) {
  if (!url || !url[0] || strlen(url) >= sizeof(_macroUrl)) return false;
  strlcpy(_macroUrl, url, sizeof(_macroUrl));
  _macroIndex = 0;
  if (!sendKeyboardShortcut(KEY_LEFT_GUI, 0, KEY_SPACE)) {
    _macroUrl[0] = '\0';
    return false;
  }
  _macroStep = MacroStep::WaitForSpotlight;
  _macroAtMs = nowMs + 500;
  return true;
}

void MacRemoteController::cancelMacro() {
  _macroStep = MacroStep::Idle;
  _macroAtMs = 0;
  _macroIndex = 0;
  _macroUrl[0] = '\0';
}

bool MacRemoteController::sendKeyboardShortcut(uint8_t firstModifier,
                                               uint8_t secondModifier,
                                               uint8_t key) {
  bool sent = _keyboard.press(firstModifier) == 1;
  if (secondModifier) {
    sent = _keyboard.press(secondModifier) == 1 && sent;
  }
  sent = _keyboard.press(key) == 1 && sent;
  _keyboard.releaseAll();
  return sent;
}

bool MacRemoteController::sendConsumerControl(uint16_t usage) {
  const bool pressed = _consumerControl.press(usage) == 1;
  const bool released = _consumerControl.release() == 1;
  return pressed && released;
}
