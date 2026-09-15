#include "UsbMassStorageController.h"

#include <USB.h>
#include <hal/usb_serial_jtag_ll.h>

#include "SdCardController.h"

UsbMassStorageController *UsbMassStorageController::_instance = nullptr;

UsbMassStorageController::UsbMassStorageController(SdCardController &sdCard,
                                                   USBMSC &msc)
  : _sdCard(sdCard),
    _msc(msc),
    _stateMux(portMUX_INITIALIZER_UNLOCKED),
    _state(UsbDriveState::WaitingForCard),
    _ejectPending(false),
    _disconnectPending(false),
    _ioErrorPending(false),
    _configured(false),
    _mediumPresent(false),
    _acceptIo(false),
    _callbacksInFlight(0) {
}

bool UsbMassStorageController::begin() {
  _instance = this;
  _msc.mediaPresent(false);
  _msc.isWritable(true);
  _msc.vendorID("ESPMedia");
  _msc.productID("ESP Media SD");
  _msc.productRevision("1.0");
  _msc.onRead(onRead);
  _msc.onWrite(onWrite);
  _msc.onStartStop(onStartStop);
  USB.onEvent(onUsbEvent);

  // CDC/MSC/DFU On Boot are disabled, so the native peripheral is still
  // dormant here. Configure both the USB device and its MSC LUN before the
  // transfer boot explicitly makes the second connector visible to the host.
  USB.manufacturerName("ESPMedia");
  USB.productName("ESP Media SD");
  USB.firmwareVersion(0x0100);

  // Normal boot disconnects the S3 USB-Serial/JTAG pad. That register can
  // survive ESP.restart(), so restore the physical pad before TinyUSB claims
  // the internal PHY for its MSC device.
  usb_serial_jtag_ll_phy_enable_pad(true);
  if (!USB.begin()) {
    portENTER_CRITICAL(&_stateMux);
    _state = UsbDriveState::UsbInitError;
    portEXIT_CRITICAL(&_stateMux);
    return false;
  }

  Serial.println(F("Native USB MSC initialized."));
  _configured = true;
  return true;
}

void UsbMassStorageController::tick() {
  if (!_configured) return;

  bool eject = false;
  bool disconnected = false;
  bool ioError = false;
  portENTER_CRITICAL(&_stateMux);
  eject = _ejectPending;
  disconnected = _disconnectPending;
  ioError = _ioErrorPending;
  _ejectPending = false;
  _disconnectPending = false;
  _ioErrorPending = false;
  portEXIT_CRITICAL(&_stateMux);

  if (eject) {
    withdrawMedium(UsbDriveState::Ejected);
    return;
  }
  if (disconnected) {
    withdrawMedium(UsbDriveState::Disconnected);
    return;
  }
  if (ioError) {
    withdrawMedium(UsbDriveState::IoError);
    return;
  }
  const UsbDriveState current = state();
  if ((current == UsbDriveState::Ready || current == UsbDriveState::Active) &&
      !_sdCard.isCardPresent()) {
    withdrawMedium(UsbDriveState::IoError);
    return;
  }
  if (current == UsbDriveState::WaitingForCard && _sdCard.isMounted()) {
    if (!exposeCard()) {
      withdrawMedium(UsbDriveState::IoError);
    }
  }
}

UsbDriveState UsbMassStorageController::state() const {
  portENTER_CRITICAL(&_stateMux);
  const UsbDriveState current = _state;
  portEXIT_CRITICAL(&_stateMux);
  return current;
}

bool UsbMassStorageController::cancelBeforeHostAccess() {
  const UsbDriveState current = state();
  if (current != UsbDriveState::WaitingForCard && current != UsbDriveState::Ready &&
      current != UsbDriveState::IoError) {
    return false;
  }
  withdrawMedium(UsbDriveState::Cancelled);
  return true;
}

bool UsbMassStorageController::exposeCard() {
  _msc.mediaPresent(false);
  _mediumPresent = false;
  if (!_sdCard.beginUsbExport()) return false;

  const uint32_t blocks = _sdCard.usbBlockCount();
  const uint16_t blockSize = _sdCard.usbBlockSize();
  if (!_msc.begin(blocks, blockSize)) {
    _sdCard.endUsbExport();
    return false;
  }

  portENTER_CRITICAL(&_stateMux);
  _state = UsbDriveState::Ready;
  _acceptIo = true;
  portEXIT_CRITICAL(&_stateMux);
  _mediumPresent = true;
  _msc.isWritable(true);
  _msc.mediaPresent(true);
  return true;
}

void UsbMassStorageController::withdrawMedium(UsbDriveState terminalState) {
  portENTER_CRITICAL(&_stateMux);
  _acceptIo = false;
  portEXIT_CRITICAL(&_stateMux);
  if (_mediumPresent) {
    _msc.mediaPresent(false);
    _mediumPresent = false;
  }

  // mediaPresent(false) prevents new commands. A device-side cancellation can
  // still race a callback that already started, so wait for that bounded raw
  // transfer before releasing the block driver.
  bool drained = false;
  const uint32_t waitStartedMs = millis();
  do {
    portENTER_CRITICAL(&_stateMux);
    drained = _callbacksInFlight == 0;
    portEXIT_CRITICAL(&_stateMux);
    if (!drained) vTaskDelay(1);
  } while (!drained && millis() - waitStartedMs < 2000);
  if (drained) _sdCard.endUsbExport();

  portENTER_CRITICAL(&_stateMux);
  _state = terminalState;
  portEXIT_CRITICAL(&_stateMux);
}

bool UsbMassStorageController::beginIo() {
  portENTER_CRITICAL(&_stateMux);
  const bool accepted = _acceptIo;
  if (accepted) {
    _callbacksInFlight++;
    if (_state == UsbDriveState::Ready) _state = UsbDriveState::Active;
  }
  portEXIT_CRITICAL(&_stateMux);
  return accepted;
}

void UsbMassStorageController::endIo(bool succeeded) {
  portENTER_CRITICAL(&_stateMux);
  if (_callbacksInFlight > 0) _callbacksInFlight--;
  if (!succeeded) _ioErrorPending = true;
  portEXIT_CRITICAL(&_stateMux);
}

int32_t UsbMassStorageController::onRead(uint32_t lba, uint32_t offset,
                                         void *buffer, uint32_t byteCount) {
  if (!_instance) return -1;
  if (!_instance->beginIo()) return -1;
  const int32_t read = _instance->_sdCard.readUsbBlocks(lba, offset, buffer, byteCount);
  _instance->endIo(read >= 0);
  return read;
}

int32_t UsbMassStorageController::onWrite(uint32_t lba, uint32_t offset,
                                          uint8_t *buffer, uint32_t byteCount) {
  if (!_instance) return -1;
  if (!_instance->beginIo()) return -1;
  const int32_t written = _instance->_sdCard.writeUsbBlocks(lba, offset, buffer, byteCount);
  _instance->endIo(written >= 0);
  return written;
}

bool UsbMassStorageController::onStartStop(uint8_t powerCondition,
                                           bool start, bool loadEject) {
  (void)powerCondition;
  (void)loadEject;
  if (!_instance) return false;
  portENTER_CRITICAL(&_instance->_stateMux);
  if (start) {
    if (_instance->_state == UsbDriveState::Ready) {
      _instance->_state = UsbDriveState::Active;
    }
  } else {
    _instance->_ejectPending = true;
  }
  portEXIT_CRITICAL(&_instance->_stateMux);
  return true;
}

void UsbMassStorageController::onUsbEvent(void *arg,
                                          esp_event_base_t eventBase,
                                          int32_t eventId,
                                          void *eventData) {
  (void)arg;
  (void)eventData;
  if (!_instance || eventBase != ARDUINO_USB_EVENTS) return;
  if (eventId == ARDUINO_USB_STOPPED_EVENT) {
    portENTER_CRITICAL(&_instance->_stateMux);
    _instance->_disconnectPending = true;
    portEXIT_CRITICAL(&_instance->_stateMux);
  }
}
