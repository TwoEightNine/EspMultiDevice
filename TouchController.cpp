#include "TouchController.h"

TouchController::TouchController(int sckPin,
                                 int misoPin,
                                 int mosiPin,
                                 int csPin,
                                 int irqPin)
  : _sckPin(sckPin),
    _misoPin(misoPin),
    _mosiPin(mosiPin),
    _csPin(csPin),
    _irqPin(irqPin),
    _screenWidth(0),
    _screenHeight(0),
    _rotation(1),
    _rawMinX(200),
    _rawMaxX(3900),
    _rawMinY(200),
    _rawMaxY(3900),
    _spiSettings(SpiFrequency, MSBFIRST, SPI_MODE0) {
}

bool TouchController::begin(uint16_t screenWidth, uint16_t screenHeight, uint8_t rotation) {
  setScreenSize(screenWidth, screenHeight);
  setRotation(rotation);
  configurePins();
  SPI.begin(_sckPin, _misoPin, _mosiPin, _csPin);
  return true;
}

bool TouchController::update() {
  TouchPoint next;
  bool touched = readPoint(next);
  bool changed = touched != _point.touched ||
                 (touched && (abs((int)next.x - (int)_point.x) > 2 ||
                              abs((int)next.y - (int)_point.y) > 2));

  _point = next;
  return changed;
}

bool TouchController::isTouched() const {
  return _point.touched;
}

TouchPoint TouchController::point() const {
  return _point;
}

void TouchController::setRotation(uint8_t rotation) {
  _rotation = rotation % 4;
}

void TouchController::setScreenSize(uint16_t screenWidth, uint16_t screenHeight) {
  _screenWidth = screenWidth;
  _screenHeight = screenHeight;
}

void TouchController::setCalibration(uint16_t rawMinX,
                                     uint16_t rawMaxX,
                                     uint16_t rawMinY,
                                     uint16_t rawMaxY) {
  _rawMinX = rawMinX;
  _rawMaxX = rawMaxX;
  _rawMinY = rawMinY;
  _rawMaxY = rawMaxY;
}

void TouchController::configurePins() {
  pinMode(_csPin, OUTPUT);
  digitalWrite(_csPin, HIGH);

  if (_irqPin >= 0) {
    pinMode(_irqPin, INPUT_PULLUP);
  }
}

bool TouchController::readPoint(TouchPoint &point) {
  point = TouchPoint();

  if (_irqPin >= 0 && digitalRead(_irqPin) == HIGH) {
    return false;
  }

  SPI.beginTransaction(_spiSettings);
  digitalWrite(_csPin, LOW);

  uint16_t z1 = readRawCommand(0xB0);
  uint16_t z2 = readRawCommand(0xC0);
  uint16_t pressure = z1 + 4095 - z2;

  if (pressure >= TouchThreshold) {
    point.rawX = readMedian(0xD0);
    point.rawY = readMedian(0x90);
    point.z = pressure;
    point.touched = true;
    mapToScreen(point);
  }

  digitalWrite(_csPin, HIGH);
  SPI.endTransaction();

  return point.touched;
}

uint16_t TouchController::readRawCommand(uint8_t command) {
  SPI.transfer(command);
  uint16_t value = SPI.transfer(0x00);
  value <<= 8;
  value |= SPI.transfer(0x00);
  return value >> 3;
}

uint16_t TouchController::readMedian(uint8_t command) {
  uint16_t samples[SamplesPerAxis];

  for (uint8_t i = 0; i < SamplesPerAxis; i++) {
    samples[i] = readRawCommand(command);
  }

  if (samples[0] > samples[1]) {
    uint16_t temp = samples[0];
    samples[0] = samples[1];
    samples[1] = temp;
  }

  if (samples[1] > samples[2]) {
    uint16_t temp = samples[1];
    samples[1] = samples[2];
    samples[2] = temp;
  }

  if (samples[0] > samples[1]) {
    uint16_t temp = samples[0];
    samples[0] = samples[1];
    samples[1] = temp;
  }

  return samples[1];
}

uint16_t TouchController::mapAxis(uint16_t value,
                                  uint16_t rawMin,
                                  uint16_t rawMax,
                                  uint16_t outMax) const {
  if (outMax == 0 || rawMin == rawMax) {
    return 0;
  }

  long mapped = map((long)value, (long)rawMin, (long)rawMax, 0, (long)outMax - 1);
  return (uint16_t)constrain(mapped, 0L, (long)outMax - 1);
}

void TouchController::mapToScreen(TouchPoint &point) const {
  switch (_rotation) {
    case 1:
      point.x = _screenWidth > 0
                  ? _screenWidth - 1 - mapAxis(point.rawY, _rawMinY, _rawMaxY, _screenWidth)
                  : 0;
      point.y = mapAxis(point.rawX, _rawMinX, _rawMaxX, _screenHeight);
      break;
    case 2:
      point.x = _screenWidth > 0
                  ? _screenWidth - 1 - mapAxis(point.rawX, _rawMinX, _rawMaxX, _screenWidth)
                  : 0;
      point.y = _screenHeight > 0
                  ? _screenHeight - 1 - mapAxis(point.rawY, _rawMinY, _rawMaxY, _screenHeight)
                  : 0;
      break;
    case 3:
      point.x = mapAxis(point.rawY, _rawMinY, _rawMaxY, _screenWidth);
      point.y = _screenHeight > 0
                  ? _screenHeight - 1 - mapAxis(point.rawX, _rawMinX, _rawMaxX, _screenHeight)
                  : 0;
      break;
    case 0:
    default:
      point.x = mapAxis(point.rawX, _rawMinX, _rawMaxX, _screenWidth);
      point.y = mapAxis(point.rawY, _rawMinY, _rawMaxY, _screenHeight);
      break;
  }
}
