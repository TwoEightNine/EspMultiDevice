#include "Mp3SeekTable.h"

void Mp3SeekTable::attach(uint32_t *storage, uint16_t capacity) {
  if (!storage || capacity < 2) {
    detach();
    return;
  }
  _offsets = storage;
  _capacity = capacity > MaxEntries ? MaxEntries : capacity;
  clear();
}

void Mp3SeekTable::detach() {
  _offsets = nullptr;
  _capacity = 0;
  clear();
}

void Mp3SeekTable::clear() {
  _count = 0;
  _interval = InitialInterval;
  _full = false;
}

void Mp3SeekTable::addFrame(uint32_t frameIndex, uint32_t offset) {
  if (!_offsets || _full || frameIndex % _interval != 0) return;
  if (_count == _capacity) {
    if (_interval >= MaxInterval) {
      _full = true;
      return;
    }
    // Keep the entries that are multiples of the doubled interval.
    uint16_t kept = 0;
    for (uint16_t i = 0; i < _count; i += 2) _offsets[kept++] = _offsets[i];
    _count = kept;
    _interval *= 2;
    if (frameIndex % _interval != 0) return;
  }
  _offsets[_count++] = offset;
}

bool Mp3SeekTable::lookup(uint32_t frameIndex, uint32_t &entryFrame, uint32_t &offset) const {
  if (!_offsets || _count == 0) return false;
  uint32_t entry = frameIndex == 0 ? 0 : (frameIndex - 1) / _interval;
  if (entry >= _count) entry = _count - 1;
  entryFrame = entry * _interval;
  offset = _offsets[entry];
  return true;
}

bool Mp3SeekTable::valid(uint32_t interval, uint32_t count, const uint32_t *offsets,
                         uint32_t dataOffset, uint32_t audioEnd, uint32_t frameCount) {
  if (!offsets || interval < InitialInterval || interval > MaxInterval ||
      (interval & (interval - 1)) != 0 || count == 0 || count > MaxEntries || frameCount == 0) {
    return false;
  }
  const uint64_t expected = ((uint64_t)frameCount + interval - 1) / interval;
  if (count != expected || offsets[0] != dataOffset || dataOffset >= audioEnd) return false;
  for (uint32_t i = 1; i < count; i++) {
    if (offsets[i] <= offsets[i - 1] || offsets[i] >= audioEnd) return false;
  }
  return true;
}

bool Mp3SeekTable::adopt(uint32_t interval, uint16_t count, uint32_t dataOffset, uint32_t audioEnd,
                         uint32_t frameCount) {
  if (!_offsets || count > _capacity ||
      !valid(interval, count, _offsets, dataOffset, audioEnd, frameCount)) {
    clear();
    return false;
  }
  _interval = interval;
  _count = count;
  _full = false;
  return true;
}
