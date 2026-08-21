#include "TextInputScreen.h"

#include <string.h>

#include "Navigator.h"

namespace {
const uint16_t InputBorderColor = TFT_DARKGREY;
const uint16_t KeyColor = 0x39c7;
const uint16_t KeyActionColor = TFT_DARKGREY;
const uint16_t KeyActiveColor = TFT_BLUE;
const uint16_t KeyPressedColor = TFT_CYAN;
const uint16_t CursorColor = TFT_WHITE;
const int16_t OuterPadding = 6;
const int16_t KeyGap = 2;
const int16_t ControlWidth = 74;
const int16_t ControlGap = 6;
const int16_t KeyboardGap = 6;
const int16_t TextPadding = 6;
const int16_t KeyboardRowCount = 5;

const char *normalRows[] = {
  "qwertyuiop",
  "asdfghjkl",
  "zxcvbnm",
  "1234567890"
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
    _symbols(false),
    _lastTouchDown(false),
    _cursorVisible(true),
    _lastBlinkMs(0),
    _pressedKey(-1),
    _pressedUntilMs(0),
    _scrollLine(0),
    _inputBounds{0, 0, 0, 0},
    _cancelBounds{0, 0, 0, 0},
    _doneBounds{0, 0, 0, 0},
    _keyCount(0),
    _lineCount(0) {
}

void TextInputScreen::onEnter() {
  _title = "Text input";
  _maskText = false;
  _text = "";
  _cursor = 0;
  _shift = false;
  _symbols = false;
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
  _symbols = false;
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
    _lastTouchDown = false;
    return;
  }

  if (_lastTouchDown) {
    return;
  }
  _lastTouchDown = true;

  if (contains(_cancelBounds, point.x, point.y)) {
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
    handleKey((uint8_t)index);
    drawKeyboard();
    drawInput();
  }
}

void TextInputScreen::tick(uint32_t nowMs) {
  bool redrawInput = false;

  if (nowMs - _lastBlinkMs >= CursorBlinkMs) {
    _lastBlinkMs = nowMs;
    _cursorVisible = !_cursorVisible;
    redrawInput = true;
  }

  if (_pressedKey >= 0 && (int32_t)(nowMs - _pressedUntilMs) >= 0) {
    int8_t oldPressedKey = _pressedKey;
    _pressedKey = -1;
    drawKey((uint8_t)oldPressedKey);
  }

  if (redrawInput) {
    drawInput();
  }
}

void TextInputScreen::draw() {
  if (!_display.isReady()) {
    return;
  }

  layout();
  _display.tft().fillScreen(TFT_BLACK);
  drawControls();
  drawInput();
  drawKeyboard();
}

void TextInputScreen::layout() {
  const int16_t width = _display.width();
  const int16_t height = _display.height();
  const int16_t controlsX = width - OuterPadding - ControlWidth;
  const int16_t inputWidth = controlsX - ControlGap - OuterPadding;
  const int16_t inputHeight = 104;

  _inputBounds = {OuterPadding, OuterPadding, inputWidth, inputHeight};
  _cancelBounds = {controlsX, OuterPadding, ControlWidth, 44};
  _doneBounds = {controlsX, OuterPadding + 44 + ControlGap, ControlWidth, 44};

  const int16_t keyboardTop = _inputBounds.y + _inputBounds.h + KeyboardGap;
  const int16_t keyboardHeight = height - keyboardTop - OuterPadding;
  const int16_t rowHeight = (keyboardHeight - KeyGap * (KeyboardRowCount - 1)) /
                            KeyboardRowCount;
  const int16_t usableWidth = width - OuterPadding * 2;
  _keyCount = 0;

  for (uint8_t row = 0; row < 4; row++) {
    layoutCharacterRow(_symbols ? symbolRows[row] : normalRows[row],
                       keyboardTop + row * (rowHeight + KeyGap), rowHeight);
  }

  const int16_t actionY = keyboardTop + 4 * (rowHeight + KeyGap);
  const int16_t shiftWidth = 48;
  const int16_t symbolsWidth = 48;
  const int16_t backspaceWidth = 64;
  const int16_t enterWidth = 56;
  const int16_t spaceWidth = usableWidth - shiftWidth - symbolsWidth -
                             backspaceWidth - enterWidth - KeyGap * 4;
  int16_t x = OuterPadding;

  addKey(_shift ? "SHIFT" : "Shift", 0, KeyAction::Shift,
         x, actionY, shiftWidth, rowHeight);
  x += shiftWidth + KeyGap;
  addKey(_symbols ? "ABC" : "#+=", 0, KeyAction::Symbols,
         x, actionY, symbolsWidth, rowHeight);
  x += symbolsWidth + KeyGap;
  addKey("Space", ' ', KeyAction::Space, x, actionY, spaceWidth, rowHeight);
  x += spaceWidth + KeyGap;
  addKey("<", 0, KeyAction::Backspace,
         x, actionY, backspaceWidth, rowHeight);
  x += backspaceWidth + KeyGap;
  addKey("Enter", '\n', KeyAction::Enter, x, actionY, enterWidth, rowHeight);
}

void TextInputScreen::layoutCharacterRow(const char *characters, int16_t y, int16_t h) {
  const uint8_t count = strlen(characters);
  if (count == 0) {
    return;
  }

  const int16_t width = _display.width() - OuterPadding * 2;
  const int16_t keyWidth = (width - KeyGap * (count - 1)) / count;
  int16_t x = OuterPadding;
  for (uint8_t i = 0; i < count; i++) {
    char label[2] = {characters[i], '\0'};
    addKey(label, characters[i], KeyAction::Character,
           x, y, keyWidth, h);
    x += keyWidth + KeyGap;
  }

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

void TextInputScreen::drawInput() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRoundRect(_inputBounds.x, _inputBounds.y,
                    _inputBounds.w, _inputBounds.h, 6, TFT_BLACK);
  tft.drawRoundRect(_inputBounds.x, _inputBounds.y,
                    _inputBounds.w, _inputBounds.h, 6, InputBorderColor);

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
  const int16_t textY = _inputBounds.y + 26;
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(_title, textX, _inputBounds.y + TextPadding, 2);
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

void TextInputScreen::drawControls() {
  drawButton(_cancelBounds, "Cancel", false);
  drawButton(_doneBounds, "Done", false);
}

void TextInputScreen::drawKeyboard() {
  for (uint8_t i = 0; i < _keyCount; i++) {
    drawKey(i);
  }
}

void TextInputScreen::drawKey(uint8_t index) {
  if (index >= _keyCount) {
    return;
  }

  const Key &key = _keys[index];
  bool active = (key.action == KeyAction::Shift && _shift) ||
                (key.action == KeyAction::Symbols && _symbols);
  uint16_t fill = active ? KeyActiveColor :
                  (_pressedKey == (int8_t)index ? KeyPressedColor :
                   (key.action == KeyAction::Character ? KeyColor : KeyActionColor));
  uint16_t textColor = (active || _pressedKey == (int8_t)index) ? TFT_BLACK : TFT_WHITE;

  TFT_eSPI &tft = _display.tft();
  tft.fillRoundRect(key.bounds.x, key.bounds.y, key.bounds.w, key.bounds.h, 5, fill);
  tft.drawRoundRect(key.bounds.x, key.bounds.y, key.bounds.w, key.bounds.h, 5, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(textColor, fill);
  tft.drawString(key.label,
                 key.bounds.x + key.bounds.w / 2,
                 key.bounds.y + key.bounds.h / 2,
                 2);
  tft.setTextDatum(TL_DATUM);
}

void TextInputScreen::drawButton(const Rect &bounds, const char *label, bool active) {
  TFT_eSPI &tft = _display.tft();
  uint16_t fill = active ? KeyActiveColor : KeyActionColor;
  tft.fillRoundRect(bounds.x, bounds.y, bounds.w, bounds.h, 6, fill);
  tft.drawRoundRect(bounds.x, bounds.y, bounds.w, bounds.h, 6, TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, fill);
  tft.drawString(label, bounds.x + bounds.w / 2, bounds.y + bounds.h / 2, 2);
  tft.setTextDatum(TL_DATUM);
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
      _shift = !_shift;
      break;
    case KeyAction::Symbols:
      _symbols = !_symbols;
      _shift = false;
      layout();
      drawKeyboard();
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
  }

  _cursorVisible = true;
  _lastBlinkMs = millis();
}

void TextInputScreen::insertCharacter(char value) {
  if (_shift && value >= 'a' && value <= 'z') {
    value = (char)(value - 'a' + 'A');
    _shift = false;
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

int8_t TextInputScreen::keyAt(int16_t x, int16_t y) const {
  for (uint8_t i = 0; i < _keyCount; i++) {
    if (contains(_keys[i].bounds, x, y)) {
      return i;
    }
  }
  return -1;
}
