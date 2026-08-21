#pragma once

#include <Arduino.h>

#include "Screen.h"
#include "SdCardController.h"

class FilesScreen : public Screen {
public:
  FilesScreen(Navigator &navigator,
              TouchController &touch,
              DisplayController &display,
              SdCardController &sdCard);
  ~FilesScreen() override;

  void onEnter() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  struct Row {
    int16_t x;
    int16_t w;
    int16_t y;
    int16_t h;
    uint8_t kind;
    uint16_t entryIndex;

    bool contains(int16_t x, int16_t touchY) const {
      return x >= this->x && x < this->x + w &&
             touchY >= y && touchY < y + h;
    }
  };

  static const int16_t ListPadding = 16;
  static const int16_t RowTop = 88;
  static const int16_t RowHeight = 30;
  static const uint8_t RowKindEntry = 0;
  static const uint8_t RowKindPrevious = 1;
  static const uint8_t RowKindNext = 2;

  SdCardController &_sdCard;
  String _currentPath;
  uint16_t _page;
  bool _sdMounted;
  bool _lastTouchDown;
  uint16_t _entryCount;
  uint16_t _pageCount;
  Row *_rows;
  uint8_t _rowCapacity;
  uint8_t _rowCount;
  uint16_t _pageSize;

  void draw();
  void drawTopBar();
  void drawPathHeader();
  void drawListing();
  void drawUnavailable();
  void refreshListingInfo();
  void ensureRowCapacity(uint8_t capacity);
  uint16_t countEntries() const;
  bool entryAt(uint16_t index, String &name, bool &isDirectory) const;
  bool isHiddenEntry(const String &path) const;
  bool isImageFile(const String &name) const;
  void openDirectory(const String &path);
  void openParentDirectory();
  String parentPath() const;
  String displayName(const String &path) const;
  String truncateLabel(const String &label, int16_t maxWidth);
  void handleSdCardEvent(SdCardEvent event);
};
