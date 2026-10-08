#pragma once

#include <Arduino.h>

#include "../../core/Screen.h"
#include "../../services/storage/SdCardController.h"
#include "../../services/storage/SecretStorage.h"

class TextViewerScreen : public Screen {
public:
  TextViewerScreen(Navigator &navigator,
                   TouchController &touch,
                   DisplayController &display,
                   SdCardController &sdCard,
                   SecretStorage &secretStorage);
  ~TextViewerScreen() override;

  void onEnter(const ScreenData &data) override;
  void onExit() override;
  bool releaseResources() override;
  void onResume() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static constexpr uint16_t ReadChunkBytes = 512;
  static constexpr uint8_t RecentPageCount = 64;
  static constexpr uint8_t MaxVisibleLines = 20;
  static constexpr uint8_t MaxLineGlyphs = 96;
  static constexpr int16_t ContentLeft = 12;
  static constexpr int16_t ContentTop = 56;
  static constexpr int16_t FooterHeight = 48;
  static constexpr uint16_t FooterColor = 0x2965;

  enum class State : uint8_t {
    Idle,
    Opening,
    Loading,
    Ready,
    Empty,
    Error
  };

  struct Glyph {
    uint32_t start;
    uint32_t end;
    char text[5];
    uint8_t length;
    bool whitespace;
  };

  struct PageMarker {
    uint32_t number;
    uint32_t offset;
    bool valid;
  };

  enum class DecodeResult : uint8_t { Glyph, Newline, NeedData, End };
  enum class FillResult : uint8_t { Ready, Busy, Failed };

  SdCardController &_sdCard;
  SecretStorage &_secretStorage;
  StorageFile _file;
  State _state;
  String _path;
  String _fileName;
  String _message;
  bool _secretAccess;
  bool _sdMounted;
  bool _lastTouchDown;
  bool _waitingForMount;
  bool _secretNeedsUnlock;
  bool _rescanning;
  bool _screenDrawn;
  uint32_t _rescanTargetPage;
  uint32_t _fileSize;
  uint32_t _contentStart;
  uint32_t _pageNumber;
  uint32_t _pageStart;
  uint32_t _nextPageStart;
  bool _hasNext;
  String _lines[MaxVisibleLines];
  uint8_t _lineCount;
  uint8_t _lineLimit;
  Glyph _glyphs[MaxLineGlyphs];
  uint8_t _glyphCount;
  String _lineText;
  uint8_t _readBuffer[ReadChunkBytes];
  uint16_t _bufferLength;
  uint16_t _bufferIndex;
  uint32_t _bufferStart;
  uint32_t _readPosition;
  bool _sourceEof;
  PageMarker _markers[RecentPageCount];
  uint8_t _nextMarker;
  uint32_t _loadingStartedMs;
  uint32_t _lastLoadingDrawMs;

  void reset();
  void closeFile();
  void openFileStep();
  void beginPage(uint32_t offset, uint32_t number, bool rescanning);
  void processPageStep();
  FillResult fillBuffer();
  DecodeResult decodeNext(Glyph &glyph, uint32_t &newlineEnd);
  bool appendGlyph(const Glyph &glyph);
  bool emitRange(uint8_t first, uint8_t end, uint32_t nextOffset);
  bool emitCurrentLine(uint32_t nextOffset);
  void shiftGlyphs(uint8_t first);
  void rebuildLineText();
  void finishAtEnd();
  void completePage();
  void fail(const String &message);
  void handleSdCardEvent(SdCardEvent event);
  void rememberPage(uint32_t number, uint32_t offset);
  bool findPage(uint32_t number, uint32_t &offset) const;
  PageMarker nearestPage(uint32_t number) const;
  void goPrevious();
  void goNext();
  void startRescan(uint32_t targetPage);
  uint8_t visibleLineLimit() const;
  int16_t contentWidth() const;
  uint8_t pagePercent() const;
  void draw();
  void drawViewport();
  void drawContent();
  void drawFooter();
  void drawLoading(uint32_t nowMs, bool clearContent);
  void drawMessage(const String &message);
  static String displayName(const String &path);
};
