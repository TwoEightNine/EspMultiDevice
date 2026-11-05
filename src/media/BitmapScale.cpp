#include "BitmapScale.h"

#include <stdlib.h>
#include <string.h>

namespace BitmapScale {

void fit(uint16_t width, uint16_t height, uint16_t maxWidth, uint16_t maxHeight,
         uint16_t &outWidth, uint16_t &outHeight) {
  if (!width || !height) { outWidth = outHeight = 0; return; }
  if (!maxWidth) maxWidth = 1;
  if (!maxHeight) maxHeight = 1;
  if (width <= maxWidth && height <= maxHeight) {
    outWidth = width;
    outHeight = height;
    return;
  }
  // Compare width/maxWidth with height/maxHeight without division.
  if (static_cast<uint32_t>(width) * maxHeight >= static_cast<uint32_t>(height) * maxWidth) {
    outWidth = maxWidth;
    uint32_t scaled = (static_cast<uint32_t>(height) * maxWidth + width / 2) / width;
    outHeight = static_cast<uint16_t>(scaled ? (scaled > maxHeight ? maxHeight : scaled) : 1);
  } else {
    outHeight = maxHeight;
    uint32_t scaled = (static_cast<uint32_t>(width) * maxHeight + height / 2) / height;
    outWidth = static_cast<uint16_t>(scaled ? (scaled > maxWidth ? maxWidth : scaled) : 1);
  }
}

uint8_t jpegShift(uint16_t width, uint16_t height, uint16_t targetWidth, uint16_t targetHeight) {
  uint8_t shift = 0;
  while (shift < 3 && reduced(width, shift + 1) >= targetWidth &&
         reduced(height, shift + 1) >= targetHeight) ++shift;
  return shift;
}

Reducer::~Reducer() { free(_sums); }

bool Reducer::begin(uint16_t sourceWidth, uint16_t sourceHeight, uint16_t *target,
                    uint16_t targetWidth, uint16_t targetHeight) {
  free(_sums);
  _sums = nullptr;
  if (!target || !sourceWidth || !sourceHeight || !targetWidth || !targetHeight ||
      targetWidth > sourceWidth || targetHeight > sourceHeight) return false;
  _sums = static_cast<uint32_t *>(calloc(static_cast<size_t>(targetWidth) * 4, sizeof(uint32_t)));
  if (!_sums) return false;
  _target = target;
  _sourceWidth = sourceWidth;
  _sourceHeight = sourceHeight;
  _targetWidth = targetWidth;
  _targetHeight = targetHeight;
  _row = -1;
  _lastY = -1;
  return true;
}

void Reducer::addRow(uint16_t y, const uint16_t *row) {
  if (!_sums || !row || y >= _sourceHeight || static_cast<int32_t>(y) <= _lastY) return;
  _lastY = y;
  const int32_t targetRow = static_cast<int32_t>(static_cast<uint32_t>(y) * _targetHeight / _sourceHeight);
  if (targetRow != _row) {
    flush();
    _row = targetRow;
  }
  uint16_t column = 0;
  uint32_t nextEdge = (static_cast<uint32_t>(column) + 1) * _sourceWidth;  // in target units
  for (uint16_t x = 0; x < _sourceWidth; ++x) {
    // x belongs to column floor(x * targetWidth / sourceWidth).
    while (static_cast<uint32_t>(x) * _targetWidth >= nextEdge) {
      ++column;
      nextEdge += _sourceWidth;
    }
    const uint16_t pixel = row[x];
    uint32_t *sum = _sums + static_cast<size_t>(column) * 4;
    sum[0] += pixel >> 11;
    sum[1] += (pixel >> 5) & 0x3f;
    sum[2] += pixel & 0x1f;
    sum[3] += 1;
  }
}

void Reducer::flush() {
  if (_row < 0 || _row >= _targetHeight) return;
  uint16_t *out = _target + static_cast<size_t>(_row) * _targetWidth;
  for (uint16_t column = 0; column < _targetWidth; ++column) {
    uint32_t *sum = _sums + static_cast<size_t>(column) * 4;
    const uint32_t count = sum[3];
    if (count) {
      const uint32_t r = (sum[0] + count / 2) / count;
      const uint32_t g = (sum[1] + count / 2) / count;
      const uint32_t b = (sum[2] + count / 2) / count;
      out[column] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
    }
    sum[0] = sum[1] = sum[2] = sum[3] = 0;
  }
}

void Reducer::finish() {
  flush();
  _row = -1;
}

}  // namespace BitmapScale
