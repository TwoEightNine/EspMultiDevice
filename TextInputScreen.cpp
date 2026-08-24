#include "TextInputScreen.h"

#include <string.h>

#include "Navigator.h"
#include "UiKit.h"

namespace {
const uint16_t InputBorderColor = TFT_DARKGREY;
const uint16_t KeyColor = 0x39c7;
const uint16_t KeyActionColor = 0x5a69;
const uint16_t KeyActiveColor = TFT_BLUE;
const uint16_t KeyPressedColor = TFT_CYAN;
const uint16_t DoneColor = 0x3666;
const uint16_t CursorColor = TFT_WHITE;
const int16_t OuterPadding = 6;
const int16_t KeyGap = 2;
const int16_t KeyboardGap = 6;
const int16_t TextPadding = 6;
const int16_t GridColumns = 10;
const int16_t KeyboardRowCount = 5;
// Adjust this value to change the height of every keyboard row.
const int16_t KeyboardRowHeight = 48;
const uint16_t ShiftHoldMs = 500;

const char *normalRows[] = {
  "1234567890",
  "qwertyuiop",
  "asdfghjkl",
  "zxcvbnm"
};

const char *symbolRows[] = {
  "1234567890",
  "@#$%^&*()-",
  "~_+=|\\<>{}",
  ".,?!'\":;/"
};
}

TextInputScreen::TextInputScreen(Navigator &navigator,
                                 TouchController &touch,
                                 DisplayController &display)
  : Screen(navigator, touch, display),
    _text(),
    _title("Text input"),
    _maskText(false),
    _cursor(0),
    _shift(false),
    _capsLock(false),
    _symbols(false),
    _shiftPressPending(false),
    _shiftPressMs(0),
    _lastTouchDown(false),
    _cursorVisible(true),
    _lastBlinkMs(0),
    _pressedKey(-1),
    _pressedUntilMs(0),
    _scrollLine(0),
    _inputBounds{0, 0, 0, 0},
    _closeBounds{0, 0, 0, 0},
    _doneBounds{0, 0, 0, 0},
    _keyboardBounds{0, 0, 0, 0},
    _keyCount(0),
    _lineCount(0) {
}

void TextInputScreen::onEnter() {
  _title = "Text input";
  _maskText = false;
  _text = "";
  _cursor = 0;
  _shift = false;
  _capsLock = false;
  _symbols = false;
  _shiftPressPending = false;
  _shiftPressMs = 0;
  _lastTouchDown = false;
  _cursorVisible = true;
  _lastBlinkMs = millis();
  _pressedKey = -1;
  _pressedUntilMs = 0;
  _scrollLine = 0;
  draw();
}

void TextInputScreen::onEnter(const ScreenData &data) {
  _title = data.kind == ScreenDataKind::TextInput && data.text.length() > 0
             ? data.text : "Text input";
  _maskText = data.kind == ScreenDataKind::TextInput && data.maskInput;
  _text = "";
  _cursor = 0;
  _shift = false;
  _capsLock = false;
  _symbols = false;
  _shiftPressPending = false;
  _shiftPressMs = 0;
  _lastTouchDown = false;
  _cursorVisible = true;
  _lastBlinkMs = millis();
  _pressedKey = -1;
  _pressedUntilMs = 0;
  _scrollLine = 0;
  draw();
}

void TextInputScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    if (_shiftPressPending) {
      finishShiftPress();
      layout();
      drawKeyboardArea();
    }
    _lastTouchDown = false;
    return;
  }

  if (_lastTouchDown) {
    return;
  }
  _lastTouchDown = true;

  if (contains(_closeBounds, point.x, point.y)) {
    _navigator.pop();
    return;
  }

  if (contains(_doneBounds, point.x, point.y)) {
    _navigator.pop(ScreenResult::withText(_text));
    return;
  }

  if (contains(_inputBounds, point.x, point.y)) {
    placeCursor(point.x, point.y);
    _cursorVisible = true;
    _lastBlinkMs = millis();
    drawInput();
    return;
  }

  int8_t index = keyAt(point.x, point.y);
  if (index >= 0) {
    _pressedKey = index;
    _pressedUntilMs = millis() + KeyFeedbackMs;
    bool oldSymbols = _symbols;
    bool oldShift = _shift;
    bool oldCapsLock = _capsLock;
    uint32_t oldCursor = _cursor;
    String oldText = _text;
    handleKey((uint8_t)index);
    if (_symbols != oldSymbols ||
        _shift != oldShift ||
        _capsLock != oldCapsLock) {
      layout();
      drawKeyboardArea();
    } else {
      drawKey((uint8_t)index);
    }
    if (_text != oldText || _cursor != oldCursor) {
      drawInput();
    }
  }
}

void TextInputScreen::tick(uint32_t nowMs) {
  if (_shiftPressPending && nowMs - _shiftPressMs >= ShiftHoldMs) {
    _shiftPressPending = false;
    _shift = false;
    _capsLock = !_capsLock;
    layout();
    drawKeyboardArea();
  }

  if (nowMs - _lastBlinkMs >= CursorBlinkMs) {
    _lastBlinkMs = nowMs;
    _cursorVisible = !_cursorVisible;
    drawInputCaretLine();
  }

  if (_pressedKey >= 0 && (int32_t)(nowMs - _pressedUntilMs) >= 0) {
    int8_t oldPressedKey = _pressedKey;
    _pressedKey = -1;
    drawKey((uint8_t)oldPressedKey);
  }

}

void TextInputScreen::draw() {
  if (!_display.isReady()) {
    return;
  }

  layout();
  _display.tft().fillScreen(TFT_BLACK);
  drawAppBar();
  drawInputFrame();
  drawInput();
  drawKeyboardArea();
}

void TextInputScreen::layout() {
  const int16_t width = _display.width();
  const int16_t height = _display.height();
  const int16_t usableWidth = width - OuterPadding * 2;
  const int16_t keyboardHeight = KeyboardRowCount * KeyboardRowHeight +
                                 KeyGap * (KeyboardRowCount - 1);
  const int16_t keyboardTop = height - OuterPadding - keyboardHeight;
  const int16_t inputHeight = keyboardTop - KeyboardGap - UiKit::AppBarHeight;
  _inputBounds = {OuterPadding, UiKit::AppBarHeight, usableWidth, inputHeight};
  _closeBounds = {0, 4, UiKit::NavigationIconSize, UiKit::NavigationIconSize};
  _keyboardBounds = {OuterPadding, keyboardTop, usableWidth, keyboardHeight};
  _keyCount = 0;

  const uint8_t characterRows = _symbols ? 4 : 3;
  for (uint8_t row = 0; row < characterRows; row++) {
    layoutCharacterRow(_symbols ? symbolRows[row] : normalRows[row], row);
  }

  if (!_symbols) {
    layoutSpannedKey("", 0, KeyAction::Shift, 3, 0, 1);
    const char *bottomLetters = "zxcvbnm";
    for (uint8_t column = 0; column < 7; column++) {
      layoutCharacterKey(bottomLetters[column], 3, column + 1);
    }
    layoutSpannedKey("", '\n', KeyAction::Enter, 3, 8, 2);
  }

  layoutSpannedKey(_symbols ? "ABC" : "#!@", 0, KeyAction::Symbols, 4, 0, 2);
  layoutSpannedKey("Space", ' ', KeyAction::Space, 4, 2, 6);
  layoutSpannedKey("Done", 0, KeyAction::Done, 4, 8, 2);
  _doneBounds = _keys[_keyCount - 1].bounds;
}

void TextInputScreen::layoutCharacterRow(const char *characters, uint8_t row) {
  const uint8_t count = strlen(characters);
  if (count == 0) {
    return;
  }

  for (uint8_t i = 0; i < count; i++) {
    layoutCharacterKey(characters[i], row, i);
  }
  if ((!_symbols && row == 2) || (_symbols && row == 3)) {
    layoutSpannedKey("", 0, KeyAction::Backspace, row, count, 1);
  }
}

void TextInputScreen::layoutCharacterKey(char value, uint8_t row, uint8_t column) {
  char label[2] = {value, '\0'};
  if (lettersUppercase() && value >= 'a' && value <= 'z') {
    label[0] = (char)(value - 'a' + 'A');
  }
  layoutSpannedKey(label, value, KeyAction::Character, row, column, 1);
}

void TextInputScreen::layoutSpannedKey(const char *label,
                                       char value,
                                       KeyAction action,
                                       uint8_t row,
                                       uint8_t column,
                                       uint8_t span) {
  const int16_t unit = (_keyboardBounds.w - KeyGap * (GridColumns - 1)) /
                       GridColumns;
  const int16_t x = _keyboardBounds.x + column * (unit + KeyGap);
  const int16_t y = _keyboardBounds.y + row * (KeyboardRowHeight + KeyGap);
  const int16_t h = KeyboardRowHeight;
  const int16_t w = unit * span + KeyGap * (span - 1);
  addKey(label, value, action, x, y, w, h);
}

void TextInputScreen::addKey(const char *label,
                             char value,
                             KeyAction action,
                             int16_t x,
                             int16_t y,
                             int16_t w,
                             int16_t h) {
  if (_keyCount >= MaxKeys) {
    return;
  }

  Key &key = _keys[_keyCount++];
  key.bounds = {x, y, w, h};
  strncpy(key.label, label, sizeof(key.label) - 1);
  key.label[sizeof(key.label) - 1] = '\0';
  key.value = value;
  key.action = action;
}

void TextInputScreen::drawAppBar() {
  UiKit::drawTopAppBar(_display.tft(), _display.width(), _title,
                       UiKit::NavigationIcon::Close);
}

void TextInputScreen::drawInputFrame() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRoundRect(_inputBounds.x, _inputBounds.y,
                    _inputBounds.w, _inputBounds.h, 6, TFT_BLACK);
  tft.drawRoundRect(_inputBounds.x, _inputBounds.y,
                    _inputBounds.w, _inputBounds.h, 6, InputBorderColor);
}

void TextInputScreen::drawInput() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(_inputBounds.x + 1, _inputBounds.y + 1,
               _inputBounds.w - 2, _inputBounds.h - 2, TFT_BLACK);

  buildVisualLines();
  const uint16_t visible = visibleLineCount();
  const uint16_t caretLine = cursorLine();
  if (caretLine < _scrollLine) {
    _scrollLine = caretLine;
  } else if (caretLine >= _scrollLine + visible) {
    _scrollLine = caretLine - visible + 1;
  }
  if (_scrollLine < 0) {
    _scrollLine = 0;
  }
  if (_lineCount > visible && _scrollLine > (int16_t)(_lineCount - visible)) {
    _scrollLine = _lineCount - visible;
  }

  const int16_t textX = _inputBounds.x + TextPadding;
  const int16_t textY = _inputBounds.y + TextPadding;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  for (uint16_t i = 0; i < visible; i++) {
    uint16_t lineIndex = _scrollLine + i;
    if (lineIndex >= _lineCount) {
      break;
    }
    String line = displayText(_text.substring(_lines[lineIndex].start, _lines[lineIndex].end));
    tft.drawString(line, textX, textY + i * LineHeight, 2);
  }

  if (_cursorVisible && caretLine >= _scrollLine &&
      caretLine < _scrollLine + visible) {
    uint32_t lineStart = _lines[caretLine].start;
    uint32_t beforeEnd = _cursor;
    if (beforeEnd > _lines[caretLine].end) {
      beforeEnd = _lines[caretLine].end;
    }
    String before = displayText(_text.substring(lineStart, beforeEnd));
    int16_t caretX = textX + textWidth(before);
    int16_t caretY = textY + (caretLine - _scrollLine) * LineHeight;
    tft.drawFastVLine(caretX, caretY + 1, LineHeight - 3, CursorColor);
  }
  tft.setTextDatum(TL_DATUM);
}

void TextInputScreen::drawInputCaretLine() {
  if (_lineCount == 0) {
    buildVisualLines();
  }

  const uint16_t visible = visibleLineCount();
  const uint16_t caretLine = cursorLine();
  if (caretLine < _scrollLine || caretLine >= _scrollLine + visible) {
    return;
  }

  TFT_eSPI &tft = _display.tft();
  const int16_t textX = _inputBounds.x + TextPadding;
  const int16_t textY = _inputBounds.y + TextPadding +
                        (caretLine - _scrollLine) * LineHeight;
  tft.fillRect(_inputBounds.x + 1, textY, _inputBounds.w - 2, LineHeight, TFT_BLACK);
  String line = displayText(_text.substring(_lines[caretLine].start,
                                            _lines[caretLine].end));
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(line, textX, textY, 2);

  if (_cursorVisible) {
    uint32_t beforeEnd = _cursor;
    if (beforeEnd > _lines[caretLine].end) {
      beforeEnd = _lines[caretLine].end;
    }
    String before = displayText(_text.substring(_lines[caretLine].start, beforeEnd));
    tft.drawFastVLine(textX + textWidth(before), textY + 1,
                      LineHeight - 3, CursorColor);
  }
  tft.setTextDatum(TL_DATUM);
}

void TextInputScreen::drawKeyboard() {
  for (uint8_t i = 0; i < _keyCount; i++) {
    drawKey(i);
  }
}

void TextInputScreen::drawKeyboardArea() {
  _display.tft().fillRect(_keyboardBounds.x, _keyboardBounds.y,
                          _keyboardBounds.w, _keyboardBounds.h, TFT_BLACK);
  drawKeyboard();
}

void TextInputScreen::drawKey(uint8_t index) {
  if (index >= _keyCount) {
    return;
  }

  const Key &key = _keys[index];
  bool active = (key.action == KeyAction::Shift && (_shift || _capsLock)) ||
                (key.action == KeyAction::Symbols && _symbols);
  uint16_t baseColor = key.action == KeyAction::Character ? KeyColor : KeyActionColor;
  if (key.action == KeyAction::Done) {
    baseColor = DoneColor;
  }
  uint16_t fill = active ? KeyActiveColor :
                  (_pressedKey == (int8_t)index ? KeyPressedColor : baseColor);
  uint16_t textColor = (active || _pressedKey == (int8_t)index) ? TFT_BLACK : TFT_WHITE;

  TFT_eSPI &tft = _display.tft();
  tft.fillRoundRect(key.bounds.x, key.bounds.y, key.bounds.w, key.bounds.h, 5, fill);
  tft.drawRoundRect(key.bounds.x, key.bounds.y, key.bounds.w, key.bounds.h, 5, TFT_WHITE);
  if (key.action == KeyAction::Backspace ||
      key.action == KeyAction::Enter ||
      key.action == KeyAction::Shift) {
    drawKeyIcon(key, textColor);
  } else {
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(textColor, fill);
    tft.drawString(key.label,
                   key.bounds.x + key.bounds.w / 2,
                   key.bounds.y + key.bounds.h / 2,
                   2);
    tft.setTextDatum(TL_DATUM);
  }
}

void TextInputScreen::drawKeyIcon(const Key &key, uint16_t color) {
  TFT_eSPI &tft = _display.tft();
  const int16_t centerX = key.bounds.x + key.bounds.w / 2;
  const int16_t centerY = key.bounds.y + key.bounds.h / 2;

  if (key.action == KeyAction::Backspace) {
    const int16_t left = centerX - 13;
    const int16_t right = centerX + 12;
    for (int8_t offset = -1; offset <= 1; offset++) {
      tft.drawLine(left + 6, centerY + offset, right, centerY + offset, color);
    }
    tft.drawLine(left + 6, centerY - 1, left, centerY, color);
    tft.drawLine(left, centerY, left + 6, centerY + 1, color);
    return;
  }

  if (key.action == KeyAction::Enter) {
    const int16_t top = centerY - 11;
    const int16_t bottom = centerY + 6;
    const int16_t left = centerX - 12;
    for (int8_t offset = -1; offset <= 1; offset++) {
      tft.drawLine(centerX + offset, top, centerX + offset, bottom, color);
      tft.drawLine(centerX + offset, bottom, left + offset, bottom, color);
    }
    tft.drawLine(left, bottom, left + 6, bottom - 5, color);
    tft.drawLine(left, bottom, left + 6, bottom + 5, color);
    return;
  }

  // Shift is a bold upward arrow. A dot below it marks Caps Lock.
  const int16_t top = centerY - 11;
  const int16_t bottom = centerY + 8;
  for (int8_t offset = -1; offset <= 1; offset++) {
    tft.drawLine(centerX + offset, bottom, centerX + offset, top + 6, color);
  }
  tft.drawLine(centerX, top, centerX - 7, top + 7, color);
  tft.drawLine(centerX, top, centerX + 7, top + 7, color);
  if (_capsLock) {
    tft.fillCircle(centerX, key.bounds.y + key.bounds.h - 6, 2, color);
  }
}

void TextInputScreen::handleKey(uint8_t index) {
  if (index >= _keyCount) {
    return;
  }

  Key &key = _keys[index];
  switch (key.action) {
    case KeyAction::Character:
      insertCharacter(key.value);
      break;
    case KeyAction::Shift:
      _shiftPressPending = true;
      _shiftPressMs = millis();
      break;
    case KeyAction::Symbols:
      _symbols = !_symbols;
      _shift = false;
      _shiftPressPending = false;
      break;
    case KeyAction::Space:
      insertCharacter(' ');
      break;
    case KeyAction::Backspace:
      deleteCharacterBeforeCursor();
      break;
    case KeyAction::Enter:
      insertCharacter('\n');
      break;
    case KeyAction::Done:
      _navigator.pop(ScreenResult::withText(_text));
      break;
  }

  _cursorVisible = true;
  _lastBlinkMs = millis();
}

void TextInputScreen::finishShiftPress() {
  if (!_shiftPressPending) {
    return;
  }

  _shiftPressPending = false;
  if (_capsLock) {
    _capsLock = false;
    _shift = false;
  } else {
    _shift = !_shift;
  }
}

void TextInputScreen::insertCharacter(char value) {
  if ((_shift || _capsLock) && value >= 'a' && value <= 'z') {
    value = (char)(value - 'a' + 'A');
    if (!_capsLock) {
      _shift = false;
    }
  }

  _text = _text.substring(0, _cursor) + String(value) + _text.substring(_cursor);
  _cursor++;
}

void TextInputScreen::deleteCharacterBeforeCursor() {
  if (_cursor == 0 || _text.length() == 0) {
    return;
  }

  _text.remove(_cursor - 1, 1);
  _cursor--;
}

void TextInputScreen::placeCursor(int16_t x, int16_t y) {
  buildVisualLines();
  uint16_t visible = visibleLineCount();
  int16_t relativeLine = (y - (_inputBounds.y + TextPadding)) / LineHeight;
  if (relativeLine < 0) {
    relativeLine = 0;
  }
  if (relativeLine >= visible) {
    relativeLine = visible - 1;
  }

  uint16_t lineIndex = _scrollLine + relativeLine;
  if (lineIndex >= _lineCount) {
    lineIndex = _lineCount == 0 ? 0 : _lineCount - 1;
  }

  const VisualLine &line = _lines[lineIndex];
  int16_t localX = x - (_inputBounds.x + TextPadding);
  if (localX <= 0) {
    _cursor = line.start;
    return;
  }

  _cursor = line.end;
  for (uint32_t index = line.start; index < line.end; index++) {
    String character = _text.substring(index, index + 1);
    int16_t midpoint = textWidth(_text.substring(line.start, index)) +
                       textWidth(character) / 2;
    if (localX < midpoint) {
      _cursor = index;
      break;
    }
  }
}

void TextInputScreen::buildVisualLines() {
  TFT_eSPI &tft = _display.tft();
  const uint16_t width = _inputBounds.w - TextPadding * 2;
  _lineCount = 0;
  uint32_t lineStart = 0;
  uint16_t lineWidth = 0;

  for (uint32_t index = 0; index < _text.length(); index++) {
    char value = _text[index];
    if (value == '\n') {
      if (_lineCount < MaxVisualLines) {
        _lines[_lineCount++] = {lineStart, index};
      }
      lineStart = index + 1;
      lineWidth = 0;
      continue;
    }

    uint16_t characterWidth = tft.textWidth(String(value), 2);
    if (lineStart != index && lineWidth + characterWidth > width) {
      if (_lineCount < MaxVisualLines) {
        _lines[_lineCount++] = {lineStart, index};
      }
      lineStart = index;
      lineWidth = 0;
    }
    lineWidth += characterWidth;
  }

  if (_lineCount < MaxVisualLines) {
    _lines[_lineCount++] = {lineStart, (uint32_t)_text.length()};
  }

  if (_lineCount == 0) {
    _lines[_lineCount++] = {0, 0};
  }
}

uint16_t TextInputScreen::cursorLine() const {
  for (uint16_t i = 0; i < _lineCount; i++) {
    if (_cursor >= _lines[i].start && _cursor <= _lines[i].end) {
      return i;
    }
  }
  return _lineCount == 0 ? 0 : _lineCount - 1;
}

uint16_t TextInputScreen::visibleLineCount() const {
  uint16_t count = (_inputBounds.h - TextPadding * 2) / LineHeight;
  return count == 0 ? 1 : count;
}

uint16_t TextInputScreen::textWidth(const String &value) const {
  return _display.tft().textWidth(value, 2);
}

String TextInputScreen::displayText(const String &value) const {
  if (!_maskText) {
    return value;
  }

  String masked;
  masked.reserve(value.length());
  for (uint16_t i = 0; i < value.length(); i++) {
    masked += value[i] == '\n' ? '\n' : '*';
  }
  return masked;
}

bool TextInputScreen::contains(const Rect &rect, int16_t x, int16_t y) const {
  return x >= rect.x && x < rect.x + rect.w &&
         y >= rect.y && y < rect.y + rect.h;
}

bool TextInputScreen::lettersUppercase() const {
  return !_symbols && (_shift || _capsLock);
}

int8_t TextInputScreen::keyAt(int16_t x, int16_t y) const {
  for (uint8_t i = 0; i < _keyCount; i++) {
    if (contains(_keys[i].bounds, x, y)) {
      return i;
    }
  }
  return -1;
}
