#pragma once

#include <Arduino.h>
#include <USBMSC.h>

class SdCardController;

enum class UsbDriveState : uint8_t {
  WaitingForCard,
  Ready,
  Active,
  Ejected,
  Disconnected,
  Cancelled,
  UsbInitError,
  IoError
};

class UsbMassStorageController {
public:
  UsbMassStorageController(SdCardController &sdCard, USBMSC &msc);

  bool begin();
  void tick();
  UsbDriveState state() const;
  bool cancelBeforeHostAccess();

private:
  static UsbMassStorageController *_instance;

  SdCardController &_sdCard;
  USBMSC &_msc;
  mutable portMUX_TYPE _stateMux;
  UsbDriveState _state;
  bool _ejectPending;
  bool _disconnectPending;
  bool _ioErrorPending;
  bool _configured;
  bool _mediumPresent;
  bool _acceptIo;
  uint16_t _callbacksInFlight;

  bool exposeCard();
  void withdrawMedium(UsbDriveState terminalState);
  bool beginIo();
  void endIo(bool succeeded);

  static int32_t onRead(uint32_t lba, uint32_t offset,
                        void *buffer, uint32_t byteCount);
  static int32_t onWrite(uint32_t lba, uint32_t offset,
                         uint8_t *buffer, uint32_t byteCount);
  static bool onStartStop(uint8_t powerCondition, bool start, bool loadEject);
  static void onUsbEvent(void *arg, esp_event_base_t eventBase,
                         int32_t eventId, void *eventData);
};
