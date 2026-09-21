#include "TouchController.h"
#include "SpiBusLock.h"

#include <FS.h>
#include <SD.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

namespace {
const uint32_t CalibrationMagic = 0x54434831UL;
const uint16_t CalibrationVersion = 1;
const char *CalibrationPath = "/.system/touch";
const char *CalibrationTempPath = "/.system/touch.tmp";
const char *CalibrationBackupPath = "/.system/touch.bak";

struct CalibrationFile {
  uint32_t magic;
  uint16_t version;
  uint16_t reserved;
  TouchCalibrationGrid grid;
  uint32_t checksum;
};

uint32_t calibrationChecksum(const CalibrationFile &file) {
  const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&file);
  size_t length = offsetof(CalibrationFile, checksum);
  uint32_t checksum = 2166136261UL;
  for (size_t i = 0; i < length; i++) {
    checksum ^= bytes[i];
    checksum *= 16777619UL;
  }
  return checksum;
}
}

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
    _hasMeshCalibration(false),
    _calibrationCapture(false),
    _pendingTouched(false),
    _pendingSinceMs(0),
    _spiSettings(SpiFrequency, MSBFIRST, SPI_MODE0) {
  memset(&_calibration, 0, sizeof(_calibration));
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
  bool candidateTouched = readPoint(next, _point.touched);
  uint32_t nowMs = millis();

  if (candidateTouched == _point.touched) {
    _pendingTouched = _point.touched;
    _pendingSinceMs = nowMs;

    if (candidateTouched &&
        (abs((int)next.x - (int)_point.x) > 2 ||
         abs((int)next.y - (int)_point.y) > 2)) {
      _point = next;
      return true;
    }

    return false;
  }

  if (_pendingTouched != candidateTouched || _pendingSinceMs == 0) {
    _pendingTouched = candidateTouched;
    _pendingSinceMs = nowMs;
    return false;
  }

  uint32_t debounceMs = candidateTouched ? PressDebounceMs : ReleaseDebounceMs;
  if (nowMs - _pendingSinceMs < debounceMs) {
    return false;
  }

  _point = next;
  _pendingSinceMs = nowMs;
  return true;
}

bool TouchController::isTouched() const {
  return _point.touched;
}

TouchPoint TouchController::point() const {
  return _point;
}

bool TouchController::hasMeshCalibration() const {
  return _hasMeshCalibration;
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

bool TouchController::setCalibration(const TouchCalibrationGrid &grid, bool persist) {
  if (!validateGrid(grid)) {
    return false;
  }

  memcpy(&_calibration, &grid, sizeof(_calibration));
  _hasMeshCalibration = true;
  return !persist || saveCalibration();
}

bool TouchController::loadCalibration() {
  _hasMeshCalibration = false;

  if (!SD.exists(CalibrationPath)) {
    return false;
  }

  File file = SD.open(CalibrationPath, FILE_READ);
  if (!file || file.size() != sizeof(CalibrationFile)) {
    if (file) {
      file.close();
    }
    return false;
  }

  CalibrationFile stored;
  size_t read = file.read(reinterpret_cast<uint8_t *>(&stored), sizeof(stored));
  file.close();

  if (read != sizeof(stored) || stored.magic != CalibrationMagic ||
      stored.version != CalibrationVersion ||
      stored.checksum != calibrationChecksum(stored) ||
      !validateGrid(stored.grid)) {
    return false;
  }

  memcpy(&_calibration, &stored.grid, sizeof(_calibration));
  _hasMeshCalibration = true;
  return true;
}

void TouchController::setCalibrationCapture(bool enabled) {
  _calibrationCapture = enabled;
}

void TouchController::configurePins() {
  pinMode(_csPin, OUTPUT);
  digitalWrite(_csPin, HIGH);

  if (_irqPin >= 0) {
    pinMode(_irqPin, INPUT_PULLUP);
  }
}

bool TouchController::readPoint(TouchPoint &point, bool holdingTouch) {
  point = TouchPoint();

  if (_irqPin >= 0 && digitalRead(_irqPin) == HIGH) {
    return false;
  }

  // The GIF loader may read SD data on the other core. Keep the shared SD /
  // touch SPI wires exclusive for this short touch transaction.
  // A GIF loader deliberately releases the bus between small reads. Wait
  // through one of those reads so a normal tap is not discarded solely
  // because touch polling landed inside an SD transaction.
  if (!SpiBusLock::lock(pdMS_TO_TICKS(25))) return false;
  SPI.beginTransaction(_spiSettings);
  digitalWrite(_csPin, LOW);

  uint16_t z1 = readRawCommand(0xB0);
  uint16_t z2 = readRawCommand(0xC0);
  uint16_t pressure = z1 + 4095 - z2;

  uint16_t minimumPressure = CalibrationNoiseFloor;

  if (pressure >= minimumPressure) {
    point.rawX = readFilteredAxis(0xD0);
    point.rawY = readFilteredAxis(0x90);
    point.z = pressure;

    uint16_t localPressureThreshold = CalibrationNoiseFloor;
    bool mapped = false;
    if (_hasMeshCalibration && !_calibrationCapture) {
      mapped = mapWithMesh(point, localPressureThreshold);
    }

    bool phantom = isPhantomCorner(point.rawX, point.rawY);
    uint16_t requiredPressure = localPressureThreshold;
    if (holdingTouch && requiredPressure > CalibrationNoiseFloor) {
      requiredPressure = (uint16_t)((uint32_t)requiredPressure * HoldPressurePercent / 100);
      if (requiredPressure < CalibrationNoiseFloor) {
        requiredPressure = CalibrationNoiseFloor;
      }
    }

    if (!phantom &&
        (_calibrationCapture ||
         (_hasMeshCalibration && mapped && pressure >= requiredPressure) ||
         (!_hasMeshCalibration && pressure >= CalibrationNoiseFloor))) {
      point.touched = true;
      if (_calibrationCapture || !_hasMeshCalibration) {
        mapToScreen(point);
      }
    }
  }

  digitalWrite(_csPin, HIGH);
  SPI.endTransaction();
  SpiBusLock::unlock();

  return point.touched;
}

uint16_t TouchController::readRawCommand(uint8_t command) {
  SPI.transfer(command);
  uint16_t value = SPI.transfer(0x00);
  value <<= 8;
  value |= SPI.transfer(0x00);
  return value >> 3;
}

uint16_t TouchController::readFilteredAxis(uint8_t command) {
  uint16_t samples[SamplesPerAxis];

  // The first conversion after selecting an ADC channel is commonly noisy.
  readRawCommand(command);

  for (uint8_t i = 0; i < SamplesPerAxis; i++) {
    samples[i] = readRawCommand(command);
  }

  uint16_t d01 = abs((int)samples[0] - (int)samples[1]);
  uint16_t d02 = abs((int)samples[0] - (int)samples[2]);
  uint16_t d12 = abs((int)samples[1] - (int)samples[2]);

  if (d01 <= d02 && d01 <= d12) {
    return (uint16_t)(((uint32_t)samples[0] + samples[1]) / 2);
  }
  if (d02 <= d01 && d02 <= d12) {
    return (uint16_t)(((uint32_t)samples[0] + samples[2]) / 2);
  }
  return (uint16_t)(((uint32_t)samples[1] + samples[2]) / 2);
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

uint16_t TouchController::mapInvertedAxis(uint16_t value,
                                          uint16_t rawMin,
                                          uint16_t rawMax,
                                          uint16_t outMax) const {
  if (outMax == 0) {
    return 0;
  }

  return outMax - 1 - mapAxis(value, rawMin, rawMax, outMax);
}

bool TouchController::isPhantomCorner(uint16_t rawX, uint16_t rawY) const {
  return rawX <= _rawMinX + PhantomCornerMargin &&
         rawY + PhantomCornerMargin >= _rawMaxY;
}

void TouchController::mapToScreen(TouchPoint &point) const {
  switch (_rotation) {
    case 1:
      point.x = mapAxis(point.rawY, _rawMinY, _rawMaxY, _screenWidth);
      point.y = mapAxis(point.rawX, _rawMinX, _rawMaxX, _screenHeight);
      break;
    case 2:
      point.x = _screenWidth > 0
                  ? _screenWidth - 1 - mapAxis(point.rawX, _rawMinX, _rawMaxX, _screenWidth)
                  : 0;
      point.y = mapAxis(point.rawY, _rawMinY, _rawMaxY, _screenHeight);
      break;
    case 3:
      point.x = mapInvertedAxis(point.rawY, _rawMinY, _rawMaxY, _screenWidth);
      point.y = _screenHeight > 0
                  ? _screenHeight - 1 - mapAxis(point.rawX, _rawMinX, _rawMaxX, _screenHeight)
                  : 0;
      break;
    case 0:
    default:
      point.x = mapAxis(point.rawX, _rawMinX, _rawMaxX, _screenWidth);
      point.y = mapInvertedAxis(point.rawY, _rawMinY, _rawMaxY, _screenHeight);
      break;
  }
}

bool TouchController::mapWithMesh(TouchPoint &point, uint16_t &localPressureThreshold) const {
  float bestError = 1000000.0f;
  float bestS = 0.0f;
  float bestT = 0.0f;
  uint8_t bestCellX = 0;
  uint8_t bestCellY = 0;
  bool found = false;

  for (uint8_t cellY = 0; cellY < 3; cellY++) {
    for (uint8_t cellX = 0; cellX < 3; cellX++) {
      float s;
      float t;
      float error;
      if (!solveCell(point.rawX, point.rawY, cellX, cellY, s, t, error)) {
        continue;
      }

      float outside = 0.0f;
      if (s < 0.0f) outside += -s;
      if (s > 1.0f) outside += s - 1.0f;
      if (t < 0.0f) outside += -t;
      if (t > 1.0f) outside += t - 1.0f;
      float score = error + outside * 100.0f;

      if (!found || score < bestError) {
        found = true;
        bestError = score;
        bestS = constrain(s, 0.0f, 1.0f);
        bestT = constrain(t, 0.0f, 1.0f);
        bestCellX = cellX;
        bestCellY = cellY;
      }
    }
  }

  if (!found) {
    mapToScreen(point);
    return false;
  }

  // The saved mesh is captured in the rotation-0 portrait coordinate space.
  // Transform its normalized result exactly like mapToScreen() so a runtime
  // TFT rotation does not bypass calibration accuracy.
  const float portraitX = (bestCellX + bestS) / 3.0f;
  const float portraitY = (bestCellY + bestT) / 3.0f;
  float normalizedX = portraitX;
  float normalizedY = portraitY;
  switch (_rotation) {
    case 1:
      normalizedX = 1.0f - portraitY;
      normalizedY = portraitX;
      break;
    case 2:
      normalizedX = 1.0f - portraitX;
      normalizedY = 1.0f - portraitY;
      break;
    case 3:
      normalizedX = portraitY;
      normalizedY = 1.0f - portraitX;
      break;
    case 0:
    default:
      break;
  }
  float screenX = normalizedX * (float)(_screenWidth - 1);
  float screenY = normalizedY * (float)(_screenHeight - 1);
  point.x = (uint16_t)constrain((long)(screenX + 0.5f), 0L, (long)_screenWidth - 1);
  point.y = (uint16_t)constrain((long)(screenY + 0.5f), 0L, (long)_screenHeight - 1);

  float z00 = _calibration.rawZ[bestCellY][bestCellX];
  float z10 = _calibration.rawZ[bestCellY][bestCellX + 1];
  float z01 = _calibration.rawZ[bestCellY + 1][bestCellX];
  float z11 = _calibration.rawZ[bestCellY + 1][bestCellX + 1];
  float top = z00 + (z10 - z00) * bestS;
  float bottom = z01 + (z11 - z01) * bestS;
  float threshold = (top + (bottom - top) * bestT) * 0.85f;
  localPressureThreshold = (uint16_t)constrain((long)(threshold + 0.5f),
                                                (long)CalibrationNoiseFloor,
                                                4095L);
  return true;
}

bool TouchController::solveCell(float rawX,
                                float rawY,
                                uint8_t cellX,
                                uint8_t cellY,
                                float &s,
                                float &t,
                                float &error) const {
  float x00 = _calibration.rawX[cellY][cellX];
  float y00 = _calibration.rawY[cellY][cellX];
  float x10 = _calibration.rawX[cellY][cellX + 1];
  float y10 = _calibration.rawY[cellY][cellX + 1];
  float x01 = _calibration.rawX[cellY + 1][cellX];
  float y01 = _calibration.rawY[cellY + 1][cellX];
  float x11 = _calibration.rawX[cellY + 1][cellX + 1];
  float y11 = _calibration.rawY[cellY + 1][cellX + 1];

  s = 0.5f;
  t = 0.5f;
  for (uint8_t i = 0; i < 8; i++) {
    float aX = x00 + (x10 - x00) * s + (x01 - x00) * t +
               (x00 - x10 - x01 + x11) * s * t;
    float aY = y00 + (y10 - y00) * s + (y01 - y00) * t +
               (y00 - y10 - y01 + y11) * s * t;
    float fX = aX - rawX;
    float fY = aY - rawY;
    float dXs = (x10 - x00) + (x00 - x10 - x01 + x11) * t;
    float dXt = (x01 - x00) + (x00 - x10 - x01 + x11) * s;
    float dYs = (y10 - y00) + (y00 - y10 - y01 + y11) * t;
    float dYt = (y01 - y00) + (y00 - y10 - y01 + y11) * s;
    float determinant = dXs * dYt - dXt * dYs;
    if (fabsf(determinant) < 0.001f) {
      return false;
    }

    s -= (fX * dYt - fY * dXt) / determinant;
    t -= (dXs * fY - dYs * fX) / determinant;
  }

  float mappedX = x00 + (x10 - x00) * s + (x01 - x00) * t +
                  (x00 - x10 - x01 + x11) * s * t;
  float mappedY = y00 + (y10 - y00) * s + (y01 - y00) * t +
                  (y00 - y10 - y01 + y11) * s * t;
  error = fabsf(mappedX - rawX) + fabsf(mappedY - rawY);
  return error <= 100.0f && s >= -0.2f && s <= 1.2f &&
         t >= -0.2f && t <= 1.2f;
}

bool TouchController::validateGrid(const TouchCalibrationGrid &grid) const {
  for (uint8_t y = 0; y < 4; y++) {
    for (uint8_t x = 0; x < 4; x++) {
      if (grid.rawX[y][x] > 4095 || grid.rawY[y][x] > 4095 || grid.rawZ[y][x] == 0) {
        return false;
      }
    }
  }

  for (uint8_t y = 0; y < 3; y++) {
    for (uint8_t x = 0; x < 3; x++) {
      float horizontalX = (float)grid.rawX[y][x + 1] - grid.rawX[y][x];
      float horizontalY = (float)grid.rawY[y][x + 1] - grid.rawY[y][x];
      float verticalX = (float)grid.rawX[y + 1][x] - grid.rawX[y][x];
      float verticalY = (float)grid.rawY[y + 1][x] - grid.rawY[y][x];
      if (fabsf(horizontalX * verticalY - horizontalY * verticalX) < 1.0f) {
        return false;
      }
    }
  }

  return true;
}

bool TouchController::saveCalibration() const {
  if (!SD.exists("/.system") && !SD.mkdir("/.system")) {
    return false;
  }

  CalibrationFile stored;
  memset(&stored, 0, sizeof(stored));
  stored.magic = CalibrationMagic;
  stored.version = CalibrationVersion;
  stored.grid = _calibration;
  stored.checksum = calibrationChecksum(stored);

  SD.remove(CalibrationTempPath);
  File file = SD.open(CalibrationTempPath, FILE_WRITE);
  if (!file) {
    return false;
  }

  size_t written = file.write(reinterpret_cast<const uint8_t *>(&stored), sizeof(stored));
  file.close();
  if (written != sizeof(stored)) {
    SD.remove(CalibrationTempPath);
    return false;
  }

  bool hadPrevious = SD.exists(CalibrationPath);
  SD.remove(CalibrationBackupPath);
  if (hadPrevious && !SD.rename(CalibrationPath, CalibrationBackupPath)) {
    SD.remove(CalibrationTempPath);
    return false;
  }

  if (!SD.rename(CalibrationTempPath, CalibrationPath)) {
    if (hadPrevious) {
      SD.rename(CalibrationBackupPath, CalibrationPath);
    }
    SD.remove(CalibrationTempPath);
    return false;
  }

  SD.remove(CalibrationBackupPath);

  return true;
}
