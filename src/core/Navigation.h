#pragma once

#include <Arduino.h>

// Small, allocation-friendly envelopes used when screens communicate through
// Navigator. New payload kinds can be added without changing screen APIs.
enum class ScreenDataKind : uint8_t {
  None,
  ImagePath,
  AudioPath,
  VideoPath,
  TextInput,
  SecretFiles,
  ExternalApp,
  NormalFolder,
  WebSearchQuery
};

enum class FileAccessMode : uint8_t { Normal, Secret };
enum class TextInputMode : uint8_t { Text, Pin4 };

struct ScreenData {
  ScreenDataKind kind;
  String text;
  String initialText;
  String previousImagePath;
  String nextImagePath;
  bool imageFullscreen;
  bool maskInput;
  FileAccessMode fileAccess;
  uint16_t pin;
  TextInputMode inputMode;

  ScreenData() : kind(ScreenDataKind::None), text(), initialText(), previousImagePath(), nextImagePath(),
                 imageFullscreen(false), maskInput(false),
                 fileAccess(FileAccessMode::Normal), pin(0), inputMode(TextInputMode::Text) {
  }

  static ScreenData imagePath(const String &path,
                              const String &previousPath = String(),
                              const String &nextPath = String()) {
    ScreenData data;
    data.kind = ScreenDataKind::ImagePath;
    data.text = path;
    data.previousImagePath = previousPath;
    data.nextImagePath = nextPath;
    return data;
  }

  static ScreenData secretImagePath(const String &path,
                                    uint16_t pin,
                                    const String &previousPath = String(),
                                    const String &nextPath = String()) {
    ScreenData data = imagePath(path, previousPath, nextPath);
    data.fileAccess = FileAccessMode::Secret;
    data.pin = pin;
    return data;
  }

  static ScreenData audioPath(const String &path) {
    ScreenData data;
    data.kind = ScreenDataKind::AudioPath;
    data.text = path;
    return data;
  }

  static ScreenData secretAudioPath(const String &path, uint16_t pin) {
    ScreenData data = audioPath(path);
    data.fileAccess = FileAccessMode::Secret;
    data.pin = pin;
    return data;
  }

  static ScreenData videoPath(const String &path) {
    ScreenData data;
    data.kind = ScreenDataKind::VideoPath;
    data.text = path;
    return data;
  }

  static ScreenData secretVideoPath(const String &path, uint16_t pin) {
    ScreenData data = videoPath(path);
    data.fileAccess = FileAccessMode::Secret;
    data.pin = pin;
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

  static ScreenData pinInput(const String &title) {
    ScreenData data = textInput(title, true);
    data.inputMode = TextInputMode::Pin4;
    return data;
  }

  static ScreenData secretFiles(uint16_t pin) {
    ScreenData data;
    data.kind = ScreenDataKind::SecretFiles;
    data.fileAccess = FileAccessMode::Secret;
    data.pin = pin;
    return data;
  }

  static ScreenData externalApp(const String &id) {
    ScreenData data;
    data.kind = ScreenDataKind::ExternalApp;
    data.text = id;
    return data;
  }

  static ScreenData normalFolder(const String &path) {
    ScreenData data;
    data.kind = ScreenDataKind::NormalFolder;
    data.text = path;
    return data;
  }

  static ScreenData webSearchQuery(const String &query) {
    ScreenData data;
    data.kind = ScreenDataKind::WebSearchQuery;
    data.text = query;
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
