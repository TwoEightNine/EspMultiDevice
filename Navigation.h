#pragma once

#include <Arduino.h>

// Small, allocation-friendly envelopes used when screens communicate through
// Navigator. New payload kinds can be added without changing screen APIs.
enum class ScreenDataKind : uint8_t {
  None,
  ImagePath
};

struct ScreenData {
  ScreenDataKind kind;
  String text;

  ScreenData() : kind(ScreenDataKind::None), text() {
  }

  static ScreenData imagePath(const String &path) {
    ScreenData data;
    data.kind = ScreenDataKind::ImagePath;
    data.text = path;
    return data;
  }
};

enum class ScreenResultKind : uint8_t {
  None,
  Text
};

struct ScreenResult {
  ScreenResultKind kind;
  String text;

  ScreenResult() : kind(ScreenResultKind::None), text() {
  }

  static ScreenResult withText(const String &value) {
    ScreenResult result;
    result.kind = ScreenResultKind::Text;
    result.text = value;
    return result;
  }
};
