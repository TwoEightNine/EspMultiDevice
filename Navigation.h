#pragma once

#include <Arduino.h>

// Small, allocation-friendly envelopes used when screens communicate through
// Navigator. New payload kinds can be added without changing screen APIs.
enum class ScreenDataKind : uint8_t {
  None,
  ImagePath,
  AudioPath,
  VideoPath,
  TextInput
};

struct ScreenData {
  ScreenDataKind kind;
  String text;
  String initialText;
  bool maskInput;

  ScreenData() : kind(ScreenDataKind::None), text(), initialText(), maskInput(false) {
  }

  static ScreenData imagePath(const String &path) {
    ScreenData data;
    data.kind = ScreenDataKind::ImagePath;
    data.text = path;
    return data;
  }

  static ScreenData audioPath(const String &path) {
    ScreenData data;
    data.kind = ScreenDataKind::AudioPath;
    data.text = path;
    return data;
  }

  static ScreenData videoPath(const String &path) {
    ScreenData data;
    data.kind = ScreenDataKind::VideoPath;
    data.text = path;
    return data;
  }

  static ScreenData textInput(const String &title, bool mask, const String &initialValue = String()) {
    ScreenData data;
    data.kind = ScreenDataKind::TextInput;
    data.text = title;
    data.initialText = initialValue;
    data.maskInput = mask;
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
