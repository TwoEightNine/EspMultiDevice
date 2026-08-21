#pragma once

#include <Arduino.h>

#include "Screen.h"

class TextInputScreen : public Screen {
public:
  TextInputScreen(Navigator &navigator,
                  TouchController &touch,
                  DisplayController &display);

  void onEnter() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const uint8_t MaxKeys = 48;
  static const uint16_t MaxVisualLines = 256;
  static const uint16_t CursorBlinkMs = 500;
  static const uint16_t KeyFeedbackMs = 120;
  static const int16_t LineHeight = 18;

  enum class KeyAction : uint8_t {
    Character,
    Shift,
    Symbols,
    Space,
    Backspace,
    Enter
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
  uint32_t _cursor;
  bool _shift;
  bool _symbols;
  bool _lastTouchDown;
  bool _cursorVisible;
  uint32_t _lastBlinkMs;
  int8_t _pressedKey;
  uint32_t _pressedUntilMs;
  int16_t _scrollLine;

  Rect _inputBounds;
  Rect _cancelBounds;
  Rect _doneBounds;
  Key _keys[MaxKeys];
  uint8_t _keyCount;
  VisualLine _lines[MaxVisualLines];
  uint16_t _lineCount;

  void draw();
  void drawInput();
  void drawControls();
  void drawKeyboard();
  void drawKey(uint8_t index);
  void layout();
  void layoutCharacterRow(const char *characters, int16_t y, int16_t h);
  void addKey(const char *label,
              char value,
              KeyAction action,
              int16_t x,
              int16_t y,
              int16_t w,
              int16_t h);

  void handleKey(uint8_t index);
  void insertCharacter(char value);
  void deleteCharacterBeforeCursor();
  void placeCursor(int16_t x, int16_t y);
  void buildVisualLines();
  uint16_t cursorLine() const;
  uint16_t visibleLineCount() const;
  uint16_t textWidth(const String &value) const;
  bool contains(const Rect &rect, int16_t x, int16_t y) const;
  int8_t keyAt(int16_t x, int16_t y) const;
  void drawButton(const Rect &bounds, const char *label, bool active);
};
