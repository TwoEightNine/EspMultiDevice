#pragma once

#include <Arduino.h>

#include "Screen.h"

class TextInputScreen : public Screen {
public:
  TextInputScreen(Navigator &navigator,
                  TouchController &touch,
                  DisplayController &display);

  void onEnter() override;
  void onEnter(const ScreenData &data) override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const uint8_t MaxKeys = 48;
  static const uint16_t MaxVisualLines = 256;
  static const uint16_t CursorBlinkMs = 500;
  static const uint16_t KeyFeedbackMs = 120;
  static const int16_t LineHeight = 21;

  enum class KeyAction : uint8_t {
    Character,
    Shift,
    Symbols,
    Space,
    Backspace,
    Enter,
    Done
  };

  struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
  };

  struct Key {
    Rect bounds;
    char label[8];
    char value;
    KeyAction action;
  };

  struct VisualLine {
    uint32_t start;
    uint32_t end;
  };

  String _text;
  String _title;
  bool _maskText;
  bool _pinInput;
  uint32_t _cursor;
  bool _shift;
  bool _capsLock;
  bool _symbols;
  bool _shiftPressPending;
  uint32_t _shiftPressMs;
  bool _lastTouchDown;
  bool _cursorVisible;
  uint32_t _lastBlinkMs;
  int8_t _pressedKey;
  uint32_t _pressedUntilMs;
  int16_t _scrollLine;

  Rect _inputBounds;
  Rect _closeBounds;
  Rect _doneBounds;
  Rect _keyboardBounds;
  Key _keys[MaxKeys];
  uint8_t _keyCount;
  VisualLine _lines[MaxVisualLines];
  uint16_t _lineCount;

  void draw();
  void drawAppBar();
  void drawInputFrame();
  void drawInput();
  void drawInputCaretLine();
  void drawKeyboard();
  void drawKeyboardArea();
  void drawKey(uint8_t index);
  void drawKeyIcon(const Key &key, uint16_t color);
  void layout();
  void layoutCharacterRow(const char *characters, uint8_t row);
  void layoutCharacterKey(char value, uint8_t row, uint8_t column);
  void layoutSpannedKey(const char *label,
                        char value,
                        KeyAction action,
                        uint8_t row,
                        uint8_t column,
                        uint8_t span);
  void addKey(const char *label,
              char value,
              KeyAction action,
              int16_t x,
              int16_t y,
              int16_t w,
              int16_t h);

  void handleKey(uint8_t index);
  void finishShiftPress();
  void insertCharacter(char value);
  void deleteCharacterBeforeCursor();
  void placeCursor(int16_t x, int16_t y);
  void buildVisualLines();
  uint16_t cursorLine() const;
  uint16_t visibleLineCount() const;
  uint16_t textWidth(const String &value) const;
  String displayText(const String &value) const;
  bool contains(const Rect &rect, int16_t x, int16_t y) const;
  int8_t keyAt(int16_t x, int16_t y) const;
  bool lettersUppercase() const;
};
