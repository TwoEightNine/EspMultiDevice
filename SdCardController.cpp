#include "SdCardController.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <string.h>

SdCardController::SdCardController(int sckPin,
                                   int misoPin,
                                   int mosiPin,
                                   int csPin,
                                   int cardDetectPin,
                                   bool cardDetectActiveLow)
  : _sckPin(sckPin),
    _misoPin(misoPin),
    _mosiPin(mosiPin),
    _csPin(csPin),
    _cardDetectPin(cardDetectPin),
    _cardDetectActiveLow(cardDetectActiveLow),
    _mounted(false),
    _autoRetrySuspended(false),
    _lastMountAttemptMs(0),
    _nextMountAttemptMs(0),
    _lastHealthCheckMs(0) {
}

bool SdCardController::begin() {
  configurePins();
  return mount();
}

SdCardEvent SdCardController::update() {
  uint32_t now = millis();
  bool present = isCardPresent();

  if (!present) {
    if (_mounted) {
      unmount();
      return SdCardEvent::Removed;
    }

    return SdCardEvent::None;
  }

  if (_mounted && now - _lastHealthCheckMs >= HealthCheckIntervalMs) {
    _lastHealthCheckMs = now;
    if (!mountedCardStillResponds()) {
      unmount();
      return SdCardEvent::Removed;
    }
  }

  if (!_mounted && !_autoRetrySuspended && now >= _nextMountAttemptMs && now - _lastMountAttemptMs >= MountRetryIntervalMs) {
    if (mount()) {
      return SdCardEvent::Mounted;
    }

    return SdCardEvent::MountFailed;
  }

  return SdCardEvent::None;
}

bool SdCardController::isMounted() const {
  return _mounted;
}

bool SdCardController::isCardPresent() const {
  if (_cardDetectPin < 0) {
    return true;
  }

  int value = digitalRead(_cardDetectPin);
  return _cardDetectActiveLow ? value == LOW : value == HIGH;
}

bool SdCardController::runReadWriteTest(const char *path) {
  if (!_mounted) {
    return false;
  }

  char firstLine[96];
  char secondLine[96];
  snprintf(firstLine, sizeof(firstLine), "ESP Media SD write test, millis=%lu\n", (unsigned long)millis());
  snprintf(secondLine, sizeof(secondLine), "ESP Media SD append test, millis=%lu\n", (unsigned long)millis());

  if (!writeTextFile(path, firstLine)) {
    return false;
  }

  if (!appendTextFile(path, secondLine)) {
    return false;
  }

  String content;
  if (!readTextFile(path, content)) {
    return false;
  }

  return content.indexOf(firstLine) >= 0 && content.indexOf(secondLine) >= 0;
}

bool SdCardController::writeTextFile(const char *path, const char *content) {
  if (!_mounted) {
    return false;
  }

  File file = SD.open(path, FILE_WRITE);
  if (!file) {
    unmount();
    return false;
  }

  size_t expected = strlen(content);
  size_t written = file.print(content);
  file.close();

  if (written != expected) {
    unmount();
    return false;
  }

  return true;
}

bool SdCardController::appendTextFile(const char *path, const char *content) {
  if (!_mounted) {
    return false;
  }

  File file = SD.open(path, FILE_APPEND);
  if (!file) {
    unmount();
    return false;
  }

  size_t expected = strlen(content);
  size_t written = file.print(content);
  file.close();

  if (written != expected) {
    unmount();
    return false;
  }

  return true;
}

bool SdCardController::readTextFile(const char *path, String &content) {
  content = "";

  if (!_mounted) {
    return false;
  }

  File file = SD.open(path, FILE_READ);
  if (!file) {
    unmount();
    return false;
  }

  while (file.available()) {
    content += (char)file.read();
  }

  file.close();
  return true;
}

bool SdCardController::mount() {
  _lastMountAttemptMs = millis();
  _nextMountAttemptMs = _lastMountAttemptMs + MountRetryIntervalMs;

  if (!isCardPresent()) {
    return false;
  }

  SD.end();
  SPI.end();
  delay(10);
  yield();

  configurePins();
  SPI.begin(_sckPin, _misoPin, _mosiPin, _csPin);

  if (!SD.begin(_csPin, SPI, SpiFrequency)) {
    SD.end();
    _mounted = false;
    if (_cardDetectPin < 0) {
      _nextMountAttemptMs = millis() + NoDetectRetryIntervalMs;
      _autoRetrySuspended = true;
      Serial.println(F("SD mount failed; auto retry suspended because no card-detect pin is configured."));
    }
    return false;
  }
  Serial.println(F("SD mounted at 20 MHz."));

  if (SD.cardType() == CARD_NONE) {
    SD.end();
    _mounted = false;
    if (_cardDetectPin < 0) {
      _nextMountAttemptMs = millis() + NoDetectRetryIntervalMs;
      _autoRetrySuspended = true;
      Serial.println(F("SD card not detected; auto retry suspended because no card-detect pin is configured."));
    }
    return false;
  }

  _mounted = true;
  _autoRetrySuspended = false;
  _lastHealthCheckMs = millis();
  return true;
}

void SdCardController::unmount() {
  SD.end();
  _mounted = false;
}

bool SdCardController::mountedCardStillResponds() {
  if (!_mounted) {
    return false;
  }

  File root = SD.open("/");
  if (!root) {
    return false;
  }

  bool ok = root.isDirectory();
  root.close();
  return ok;
}

void SdCardController::configurePins() {
  pinMode(_csPin, OUTPUT);
  digitalWrite(_csPin, HIGH);

  if (_cardDetectPin >= 0) {
    pinMode(_cardDetectPin, _cardDetectActiveLow ? INPUT_PULLUP : INPUT_PULLDOWN);
  }
}
