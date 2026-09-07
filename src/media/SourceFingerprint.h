#pragma once

#include <Arduino.h>

struct SourceFingerprint {
  uint32_t size;
  uint32_t modified;

  bool matches(const SourceFingerprint &other) const {
    return size == other.size && modified == other.modified;
  }
};
