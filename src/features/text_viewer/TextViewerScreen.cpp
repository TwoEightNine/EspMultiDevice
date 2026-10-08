#include "TextViewerScreen.h"

#include "../../app/Navigator.h"
#include "../../core/SpiBusLock.h"
#include "../../ui/UiKit.h"

#include <string.h>

namespace {
constexpr uint16_t InactiveColor = 0x8410;
constexpr uint32_t LoadingFrameMs = 80;
constexpr TickType_t SpiTimeout = pdMS_TO_TICKS(20);
}

TextViewerScreen::TextViewerScreen(Navigator &navigator,
                                   TouchController &touch,
                                   DisplayController &display,
                                   SdCardController &sdCard,
                                   SecretStorage &secretStorage)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _secretStorage(secretStorage),
    _file(),
    _state(State::Idle),
    _path(),
    _fileName(),
    _message(),
    _secretAccess(false),
    _sdMounted(false),
    _lastTouchDown(false),
    _waitingForMount(false),
    _secretNeedsUnlock(false),
    _rescanning(false),
    _screenDrawn(false),
    _rescanTargetPage(0),
    _fileSize(0),
    _contentStart(0),
    _pageNumber(1),
    _pageStart(0),
    _nextPageStart(0),
    _hasNext(false),
    _lines{},
    _lineCount(0),
    _lineLimit(1),
    _glyphs{},
    _glyphCount(0),
    _lineText(),
    _readBuffer{},
    _bufferLength(0),
    _bufferIndex(0),
    _bufferStart(0),
    _readPosition(0),
    _sourceEof(false),
    _markers{},
    _nextMarker(0),
    _loadingStartedMs(0),
    _lastLoadingDrawMs(0) {
}

TextViewerScreen::~TextViewerScreen() {
  closeFile();
}

void TextViewerScreen::onEnter(const ScreenData &data) {
  reset();
  _secretAccess = data.fileAccess == FileAccessMode::Secret;
  _path = data.kind == ScreenDataKind::TextPath ? data.text : String();
  _fileName = displayName(_path);
  if (_secretAccess && _fileName.length()) {
    String decrypted;
    if (_secretStorage.decryptName(_fileName, decrypted)) {
      _fileName = _secretStorage.displayName(decrypted);
    }
  }
  _sdMounted = _sdCard.isMounted();
  _lineLimit = visibleLineLimit();

  if (data.kind != ScreenDataKind::TextPath || _path.length() == 0) {
    fail(F("No text file selected"));
  } else if (!_sdMounted) {
    _waitingForMount = true;
    _secretNeedsUnlock = _secretAccess;
    fail(F("SD card unavailable"));
  } else if (_secretAccess && !_secretStorage.unlocked()) {
    _secretNeedsUnlock = true;
    fail(F("Close and unlock secret files again"));
  } else {
    _state = State::Opening;
    _loadingStartedMs = millis();
  }
  draw();
}

void TextViewerScreen::onExit() {
}

bool TextViewerScreen::releaseResources() {
  closeFile();
  for (uint8_t i = 0; i < MaxVisibleLines; ++i) _lines[i] = String();
  _lineText = String();
  _path = String();
  _fileName = String();
  _message = String();
  _state = State::Idle;
  _screenDrawn = false;
  return true;
}

void TextViewerScreen::onResume() {
  _lastTouchDown = false;
  draw();
}

void TextViewerScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

  if (UiKit::navigationIconContains(point.x, point.y)) {
    _navigator.pop();
    return;
  }
  if (_state != State::Ready && _state != State::Empty) return;
  const int16_t footerTop = _display.height() - FooterHeight;
  if (point.y < footerTop) return;
  if (point.x < _display.width() / 4) goPrevious();
  else if (point.x >= _display.width() * 3 / 4) goNext();
}

void TextViewerScreen::tick(uint32_t nowMs) {
  handleSdCardEvent(_sdCard.takeEvent());
  if (_state == State::Opening) openFileStep();
  else if (_state == State::Loading) processPageStep();

  if ((_state == State::Opening || _state == State::Loading) &&
      nowMs - _lastLoadingDrawMs >= LoadingFrameMs) {
    drawLoading(nowMs, false);
  }
}

void TextViewerScreen::reset() {
  closeFile();
  _state = State::Idle;
  _path = String();
  _fileName = String();
  _message = String();
  _secretAccess = false;
  _lastTouchDown = false;
  _waitingForMount = false;
  _secretNeedsUnlock = false;
  _rescanning = false;
  _screenDrawn = false;
  _rescanTargetPage = 0;
  _fileSize = 0;
  _contentStart = 0;
  _pageNumber = 1;
  _pageStart = 0;
  _nextPageStart = 0;
  _hasNext = false;
  _lineCount = 0;
  _lineLimit = 1;
  _glyphCount = 0;
  _lineText = String();
  _bufferLength = 0;
  _bufferIndex = 0;
  _bufferStart = 0;
  _readPosition = 0;
  _sourceEof = false;
  _nextMarker = 0;
  _loadingStartedMs = 0;
  _lastLoadingDrawMs = 0;
  for (uint8_t i = 0; i < MaxVisibleLines; ++i) _lines[i] = String();
  for (uint8_t i = 0; i < RecentPageCount; ++i) _markers[i] = {0, 0, false};
}

void TextViewerScreen::closeFile() {
  if (!_file) return;
  if (SpiBusLock::lock(SpiTimeout)) {
    _file.close();
    SpiBusLock::unlock();
  } else {
    _file.close();
  }
}

void TextViewerScreen::openFileStep() {
  if (!_sdMounted) return;
  if (_secretAccess && !_secretStorage.unlocked()) {
    _secretNeedsUnlock = true;
    fail(F("Close and unlock secret files again"));
    return;
  }
  if (!SpiBusLock::lock(SpiTimeout)) return;
  _file = _secretStorage.open(_path, FILE_READ, _secretAccess);
  if (!_file) {
    SpiBusLock::unlock();
    fail(F("Unable to open text file"));
    return;
  }
  const size_t size = _file.size();
  _fileSize = size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(size);
  uint8_t prefix[3] = {};
  const int prefixLength = _file.read(prefix, _fileSize < sizeof(prefix) ? _fileSize : sizeof(prefix));
  if (prefixLength < 0) {
    _file.close();
    SpiBusLock::unlock();
    fail(F("Unable to read text file"));
    return;
  }
  if (prefixLength >= 2 && ((prefix[0] == 0xff && prefix[1] == 0xfe) ||
                            (prefix[0] == 0xfe && prefix[1] == 0xff))) {
    _file.close();
    SpiBusLock::unlock();
    fail(F("UTF-16 text is not supported"));
    return;
  }
  _contentStart = prefixLength >= 3 && prefix[0] == 0xef && prefix[1] == 0xbb && prefix[2] == 0xbf ? 3 : 0;
  const bool positioned = _file.seek(_contentStart);
  SpiBusLock::unlock();
  if (!positioned) {
    fail(F("Unable to seek text file"));
    return;
  }
  if (_contentStart >= _fileSize) {
    _state = State::Empty;
    _pageNumber = 1;
    _pageStart = _contentStart;
    _nextPageStart = _contentStart;
    drawViewport();
    return;
  }
  beginPage(_contentStart, 1, false);
}

void TextViewerScreen::beginPage(uint32_t offset, uint32_t number, bool rescanning) {
  _state = State::Loading;
  _rescanning = rescanning;
  _pageNumber = number;
  _pageStart = offset;
  _nextPageStart = offset;
  _hasNext = false;
  _lineCount = 0;
  _glyphCount = 0;
  _lineText = String();
  _bufferLength = 0;
  _bufferIndex = 0;
  _bufferStart = offset;
  _readPosition = offset;
  _sourceEof = offset >= _fileSize;
  for (uint8_t i = 0; i < MaxVisibleLines; ++i) _lines[i] = String();
  rememberPage(number, offset);
  _loadingStartedMs = millis();
  _lastLoadingDrawMs = 0;
  drawLoading(_loadingStartedMs, true);
}

void TextViewerScreen::processPageStep() {
  bool readUsed = false;
  for (uint16_t work = 0; work < ReadChunkBytes * 2; ++work) {
    Glyph glyph{};
    uint32_t newlineEnd = 0;
    const DecodeResult result = decodeNext(glyph, newlineEnd);
    if (result == DecodeResult::NeedData) {
      if (readUsed) return;
      const FillResult filled = fillBuffer();
      if (filled == FillResult::Busy) return;
      if (filled == FillResult::Failed) {
        fail(F("Unable to read text file"));
        return;
      }
      readUsed = true;
      continue;
    }
    if (result == DecodeResult::End) {
      finishAtEnd();
      return;
    }
    if (result == DecodeResult::Newline) {
      if (emitCurrentLine(newlineEnd)) {
        completePage();
        return;
      }
      continue;
    }
    if (appendGlyph(glyph)) {
      completePage();
      return;
    }
  }
}

TextViewerScreen::FillResult TextViewerScreen::fillBuffer() {
  const uint16_t tail = _bufferLength - _bufferIndex;
  if (tail && _bufferIndex) memmove(_readBuffer, _readBuffer + _bufferIndex, tail);
  _bufferStart += _bufferIndex;
  _bufferLength = tail;
  _bufferIndex = 0;
  if (_readPosition >= _fileSize) {
    _sourceEof = true;
    return FillResult::Ready;
  }
  if (!SpiBusLock::lock(SpiTimeout)) return FillResult::Busy;
  const bool positioned = _file.seek(_readPosition);
  int count = positioned ? _file.read(_readBuffer + tail, ReadChunkBytes - tail) : -1;
  SpiBusLock::unlock();
  if (!positioned || count <= 0) return FillResult::Failed;
  _bufferLength += static_cast<uint16_t>(count);
  _readPosition += static_cast<uint32_t>(count);
  _sourceEof = _readPosition >= _fileSize;
  return FillResult::Ready;
}

TextViewerScreen::DecodeResult TextViewerScreen::decodeNext(Glyph &glyph, uint32_t &newlineEnd) {
  if (_bufferIndex >= _bufferLength) return _sourceEof ? DecodeResult::End : DecodeResult::NeedData;
  const uint16_t remaining = _bufferLength - _bufferIndex;
  const uint8_t first = _readBuffer[_bufferIndex];
  glyph.start = _bufferStart + _bufferIndex;
  glyph.end = glyph.start + 1;
  glyph.length = 1;
  glyph.text[0] = '?';
  glyph.text[1] = 0;
  glyph.whitespace = false;

  if (first == '\r') {
    if (remaining == 1 && !_sourceEof) return DecodeResult::NeedData;
    ++_bufferIndex;
    if (_bufferIndex < _bufferLength && _readBuffer[_bufferIndex] == '\n') ++_bufferIndex;
    newlineEnd = _bufferStart + _bufferIndex;
    return DecodeResult::Newline;
  }
  if (first == '\n') {
    ++_bufferIndex;
    newlineEnd = _bufferStart + _bufferIndex;
    return DecodeResult::Newline;
  }
  if (first == '\t') {
    ++_bufferIndex;
    memcpy(glyph.text, "    ", 5);
    glyph.length = 4;
    glyph.whitespace = true;
    return DecodeResult::Glyph;
  }
  if (first < 0x80) {
    ++_bufferIndex;
    glyph.text[0] = first >= 32 && first != 0x7f ? static_cast<char>(first) : '?';
    glyph.whitespace = first == ' ';
    return DecodeResult::Glyph;
  }

  uint8_t length = first >= 0xc2 && first <= 0xdf ? 2 :
                   (first >= 0xe0 && first <= 0xef ? 3 :
                   (first >= 0xf0 && first <= 0xf4 ? 4 : 0));
  if (length == 0) {
    ++_bufferIndex;
    return DecodeResult::Glyph;
  }
  if (remaining < length) {
    if (!_sourceEof) return DecodeResult::NeedData;
    ++_bufferIndex;
    return DecodeResult::Glyph;
  }
  uint32_t codepoint = first & (length == 2 ? 0x1f : (length == 3 ? 0x0f : 0x07));
  bool valid = true;
  for (uint8_t i = 1; i < length; ++i) {
    const uint8_t next = _readBuffer[_bufferIndex + i];
    if ((next & 0xc0) != 0x80) {
      valid = false;
      break;
    }
    codepoint = (codepoint << 6) | (next & 0x3f);
  }
  if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
      (length == 4 && codepoint < 0x10000) ||
      (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff) valid = false;
  if (!valid) {
    ++_bufferIndex;
    return DecodeResult::Glyph;
  }
  glyph.length = length;
  glyph.end = glyph.start + length;
  for (uint8_t i = 0; i < length; ++i) glyph.text[i] = static_cast<char>(_readBuffer[_bufferIndex + i]);
  glyph.text[length] = 0;
  _bufferIndex += length;
  return DecodeResult::Glyph;
}

bool TextViewerScreen::appendGlyph(const Glyph &glyph) {
  if (_glyphCount >= MaxLineGlyphs) {
    const uint32_t nextOffset = glyph.start;
    if (emitRange(0, _glyphCount, nextOffset)) return true;
    _glyphCount = 0;
    _lineText = String();
  }
  _glyphs[_glyphCount++] = glyph;
  for (uint8_t i = 0; i < glyph.length; ++i) _lineText += glyph.text[i];
  if (UiText::measure(_lineText, UiText::Size::Content) <= contentWidth()) return false;

  bool sawContent = false;
  int16_t breakAt = -1;
  for (uint8_t i = 0; i < _glyphCount; ++i) {
    if (_glyphs[i].whitespace && sawContent) breakAt = i;
    else if (!_glyphs[i].whitespace) sawContent = true;
  }
  if (breakAt >= 0) {
    uint8_t runStart = static_cast<uint8_t>(breakAt);
    while (runStart > 0 && _glyphs[runStart - 1].whitespace) --runStart;
    uint8_t remainder = static_cast<uint8_t>(breakAt + 1);
    while (remainder < _glyphCount && _glyphs[remainder].whitespace) ++remainder;
    const uint32_t nextOffset = remainder < _glyphCount ? _glyphs[remainder].start
                                                        : _glyphs[breakAt].end;
    if (emitRange(0, runStart, nextOffset)) return true;
    shiftGlyphs(remainder);
    return false;
  }

  if (_glyphCount == 1) {
    const uint32_t nextOffset = glyph.end;
    if (emitRange(0, 1, nextOffset)) return true;
    _glyphCount = 0;
    _lineText = String();
    return false;
  }
  const uint8_t remainder = _glyphCount - 1;
  const uint32_t nextOffset = _glyphs[remainder].start;
  if (emitRange(0, remainder, nextOffset)) return true;
  shiftGlyphs(remainder);
  return false;
}

bool TextViewerScreen::emitRange(uint8_t first, uint8_t end, uint32_t nextOffset) {
  if (_lineCount >= _lineLimit) return true;
  String line;
  for (uint8_t i = first; i < end; ++i) {
    for (uint8_t j = 0; j < _glyphs[i].length; ++j) line += _glyphs[i].text[j];
  }
  _lines[_lineCount++] = UiText::normalizeForFont(line, UiText::Size::Content);
  if (_lineCount < _lineLimit) return false;
  _nextPageStart = nextOffset;
  _hasNext = nextOffset < _fileSize;
  return true;
}

bool TextViewerScreen::emitCurrentLine(uint32_t nextOffset) {
  const bool complete = emitRange(0, _glyphCount, nextOffset);
  _glyphCount = 0;
  _lineText = String();
  return complete;
}

void TextViewerScreen::shiftGlyphs(uint8_t first) {
  const uint8_t retained = first < _glyphCount ? _glyphCount - first : 0;
  for (uint8_t i = 0; i < retained; ++i) _glyphs[i] = _glyphs[first + i];
  _glyphCount = retained;
  rebuildLineText();
}

void TextViewerScreen::rebuildLineText() {
  _lineText = String();
  for (uint8_t i = 0; i < _glyphCount; ++i) {
    for (uint8_t j = 0; j < _glyphs[i].length; ++j) _lineText += _glyphs[i].text[j];
  }
}

void TextViewerScreen::finishAtEnd() {
  if (_glyphCount > 0) {
    if (emitCurrentLine(_fileSize)) {
      _hasNext = false;
      _nextPageStart = _fileSize;
    }
  }
  _hasNext = false;
  _nextPageStart = _fileSize;
  completePage();
}

void TextViewerScreen::completePage() {
  if (_hasNext) rememberPage(_pageNumber + 1, _nextPageStart);
  if (_rescanning && _pageNumber < _rescanTargetPage) {
    if (!_hasNext) {
      fail(F("Unable to find previous page"));
      return;
    }
    beginPage(_nextPageStart, _pageNumber + 1, true);
    return;
  }
  _rescanning = false;
  _state = _lineCount == 0 ? State::Empty : State::Ready;
  drawViewport();
}

void TextViewerScreen::fail(const String &message) {
  closeFile();
  _message = message;
  _state = State::Error;
  _rescanning = false;
  if (_screenDrawn) drawViewport();
}

void TextViewerScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::None) return;
  if (event == SdCardEvent::Mounted) {
    _sdMounted = true;
    if (!_waitingForMount) return;
    _waitingForMount = false;
    if (_secretAccess || _secretNeedsUnlock) {
      _secretNeedsUnlock = true;
      fail(F("Close and unlock secret files again"));
      return;
    }
    closeFile();
    _nextMarker = 0;
    for (uint8_t i = 0; i < RecentPageCount; ++i) _markers[i] = {0, 0, false};
    _state = State::Opening;
    _loadingStartedMs = millis();
    drawViewport();
    return;
  }

  _sdMounted = false;
  closeFile();
  _waitingForMount = true;
  _secretNeedsUnlock = _secretAccess;
  fail(event == SdCardEvent::Removed ? F("SD card removed") : F("SD card unavailable"));
}

void TextViewerScreen::rememberPage(uint32_t number, uint32_t offset) {
  for (uint8_t i = 0; i < RecentPageCount; ++i) {
    if (_markers[i].valid && _markers[i].number == number) {
      _markers[i].offset = offset;
      return;
    }
  }
  _markers[_nextMarker] = {number, offset, true};
  _nextMarker = (_nextMarker + 1) % RecentPageCount;
}

bool TextViewerScreen::findPage(uint32_t number, uint32_t &offset) const {
  if (number == 1) {
    offset = _contentStart;
    return true;
  }
  for (uint8_t i = 0; i < RecentPageCount; ++i) {
    if (_markers[i].valid && _markers[i].number == number) {
      offset = _markers[i].offset;
      return true;
    }
  }
  return false;
}

TextViewerScreen::PageMarker TextViewerScreen::nearestPage(uint32_t number) const {
  PageMarker nearest{1, _contentStart, true};
  for (uint8_t i = 0; i < RecentPageCount; ++i) {
    if (_markers[i].valid && _markers[i].number <= number &&
        _markers[i].number > nearest.number) nearest = _markers[i];
  }
  return nearest;
}

void TextViewerScreen::goPrevious() {
  if (_pageNumber <= 1) return;
  const uint32_t target = _pageNumber - 1;
  uint32_t offset = 0;
  if (findPage(target, offset)) beginPage(offset, target, false);
  else startRescan(target);
}

void TextViewerScreen::goNext() {
  if (!_hasNext) return;
  beginPage(_nextPageStart, _pageNumber + 1, false);
}

void TextViewerScreen::startRescan(uint32_t targetPage) {
  _rescanTargetPage = targetPage;
  const PageMarker start = nearestPage(targetPage);
  beginPage(start.offset, start.number, true);
}

uint8_t TextViewerScreen::visibleLineLimit() const {
  const int16_t available = _display.height() - FooterHeight - ContentTop;
  int16_t count = available / UiText::lineHeight(UiText::Size::Content);
  if (count < 1) count = 1;
  if (count > MaxVisibleLines) count = MaxVisibleLines;
  return static_cast<uint8_t>(count);
}

int16_t TextViewerScreen::contentWidth() const {
  const int16_t width = _display.width() - ContentLeft * 2;
  return width > 1 ? width : 1;
}

uint8_t TextViewerScreen::pagePercent() const {
  if (_fileSize <= _contentStart) return 100;
  const uint32_t position = _state == State::Ready ? _nextPageStart : _pageStart;
  const uint32_t contentBytes = _fileSize - _contentStart;
  const uint32_t shown = position > _contentStart ? position - _contentStart : 0;
  const uint32_t percent = static_cast<uint32_t>((static_cast<uint64_t>(shown) * 100) / contentBytes);
  return percent > 100 ? 100 : static_cast<uint8_t>(percent);
}

void TextViewerScreen::draw() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), _fileName.length() ? _fileName : String(F("Text")));
  _screenDrawn = true;
  drawViewport();
}

void TextViewerScreen::drawViewport() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t contentBottom = _display.height() - FooterHeight;
  tft.fillRect(0, UiKit::AppBarHeight, _display.width(),
               contentBottom - UiKit::AppBarHeight, TFT_BLACK);
  if (_state == State::Opening || _state == State::Loading) drawLoading(millis(), false);
  else if (_state == State::Ready) drawContent();
  else if (_state == State::Empty) drawMessage(F("Empty file"));
  else if (_state == State::Error) drawMessage(_message);
  drawFooter();
}

void TextViewerScreen::drawContent() {
  TFT_eSPI &tft = _display.tft();
  const int16_t lineHeight = UiText::lineHeight(UiText::Size::Content);
  for (uint8_t i = 0; i < _lineCount; ++i) {
    UiText::draw(tft, _lines[i], ContentLeft, ContentTop + i * lineHeight,
                 TFT_WHITE, TFT_BLACK, UiText::Size::Content, TL_DATUM);
  }
}

void TextViewerScreen::drawFooter() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t top = _display.height() - FooterHeight;
  const int16_t quarter = _display.width() / 4;
  const int16_t previousWidth = quarter;
  const int16_t nextX = _display.width() - quarter;
  const bool ready = _state == State::Ready || _state == State::Empty;
  const bool previousActive = ready && _pageNumber > 1;
  const bool nextActive = ready && _hasNext;
  tft.fillRect(0, top, _display.width(), FooterHeight, FooterColor);
  tft.fillRect(previousWidth - 1, top, 1, FooterHeight, TFT_DARKGREY);
  tft.fillRect(nextX, top, 1, FooterHeight, TFT_DARKGREY);
  UiKit::drawPageArrow(tft, previousWidth / 2, top + FooterHeight / 2,
                       true, false, previousActive ? TFT_WHITE : InactiveColor, FooterColor);
  UiKit::drawPageArrow(tft, nextX + quarter / 2, top + FooterHeight / 2,
                       true, true, nextActive ? TFT_WHITE : InactiveColor, FooterColor);
  String label = String(F("Page ")) + String(_pageNumber);
  if (ready) label += String(F(" \xC2\xB7 ")) + String(pagePercent()) + F("%");
  UiText::draw(tft, label, _display.width() / 2, top + FooterHeight / 2,
               TFT_WHITE, FooterColor, UiText::Size::Compact, MC_DATUM);
}

void TextViewerScreen::drawLoading(uint32_t nowMs, bool clearContent) {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  const int16_t contentBottom = _display.height() - FooterHeight;
  if (clearContent) tft.fillRect(0, UiKit::AppBarHeight, _display.width(), contentBottom - UiKit::AppBarHeight, TFT_BLACK);
  tft.fillRect(0, 170, _display.width(), 95, TFT_BLACK);
  const String label = _rescanning ? String(F("Finding page ")) + String(_rescanTargetPage)
                                   : String(F("Loading text..."));
  UiText::draw(tft, label, _display.width() / 2, 190, TFT_LIGHTGREY, TFT_BLACK,
               UiText::Size::Content, MC_DATUM);
  UiKit::drawProgressBar(tft, 24, 220, _display.width() - 48, 12, 0, true,
                         TFT_BLUE, TFT_DARKGREY, TFT_BLACK, true,
                         nowMs - _loadingStartedMs);
  _lastLoadingDrawMs = nowMs;
}

void TextViewerScreen::drawMessage(const String &message) {
  UiText::draw(_display.tft(), UiKit::ellipsize(message, _display.width() - 32,
                                                UiText::Size::Content),
               _display.width() / 2,
               UiKit::AppBarHeight + (_display.height() - FooterHeight - UiKit::AppBarHeight) / 2,
               TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Content, MC_DATUM);
}

String TextViewerScreen::displayName(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(slash + 1);
}
