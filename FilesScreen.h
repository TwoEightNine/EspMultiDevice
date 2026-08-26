#pragma once

#include <Arduino.h>

#include "FileFormats.h"
#include "Screen.h"
#include "SdCardController.h"
#include "UiKit.h"

class FileThumbnailWorker;

class FilesScreen : public Screen {
public:
  FilesScreen(Navigator &navigator,
              TouchController &touch,
              DisplayController &display,
              SdCardController &sdCard);
  ~FilesScreen() override;

  void onEnter() override;
  void onExit() override;
  void onResume() override;
  void onResult(const ScreenResult &result) override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static const uint8_t MaxCachedDirectories = 8;
  static const uint8_t MaxCachedThumbnails = 12;
  static const int16_t RowTop = 88;
  static const int16_t FooterHeight = 42;
  static const int16_t ActionWidth = 48;
  static const uint16_t InvalidEntryIndex = 0xffff;
  static const uint32_t DeleteHoldMs = 1000;

  struct Rect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;

    bool contains(int16_t touchX, int16_t touchY) const {
      return touchX >= x && touchX < x + w && touchY >= y && touchY < y + h;
    }
  };

  struct Entry {
    String name;
    FileFormats::Format format;
    bool directory;
  };

  struct DirectoryCache {
    String path;
    Entry *entries;
    uint16_t count;
    uint32_t lastUsed;
    bool used;
  };

  struct Row {
    Rect bounds;
    uint16_t entryIndex;
    bool parent;
  };

  struct Thumbnail {
    String path;
    uint16_t *pixels;
    uint32_t lastUsed;
    uint32_t generation;
    bool used;
    bool pending;
    bool failed;
  };

  enum class MenuState : uint8_t {
    None,
    Actions,
    DeleteHolding
  };

  SdCardController &_sdCard;
  String _currentPath;
  String _pendingRenamePath;
  String _message;
  uint32_t _messageUntilMs;
  uint16_t _page;
  bool _sdMounted;
  bool _lastTouchDown;
  bool _retainCacheWhileCovered;
  DirectoryCache _cache[MaxCachedDirectories];
  Thumbnail _thumbnails[MaxCachedThumbnails];
  FileThumbnailWorker *_thumbnailWorker;
  uint32_t _listingGeneration;
  DirectoryCache *_listing;
  Row *_rows;
  uint8_t _rowCapacity;
  uint8_t _rowCount;
  uint8_t _rowSlots;
  uint16_t _pageCount;
  Rect _previousBounds;
  Rect _nextBounds;
  Rect _menuBounds;
  Rect _renameBounds;
  Rect _deleteBounds;
  MenuState _menuState;
  uint16_t _menuEntryIndex;
  uint32_t _deleteStartedMs;
  uint32_t _lastDeleteDrawMs;

  void draw();
  void drawTopBar();
  void drawPathHeader();
  void drawListing();
  void redrawListingArea();
  void drawRow(const Row &row);
  void redrawRowsIntersecting(const Rect &bounds);
  void drawFooter();
  void drawContextMenu();
  void drawStatus();
  void drawUnavailable();
  void drawVerticalEllipsis(int16_t centerX, int16_t centerY, uint16_t color) const;
  void drawPagingArrow(int16_t centerX, int16_t centerY, bool forward, uint16_t color) const;
  Rect statusBounds() const;
  void queueVisibleThumbnails();
  Thumbnail *thumbnailFor(const String &path);
  Thumbnail *reserveThumbnail(const String &path);
  void collectThumbnailResults();
  void clearThumbnails();
  void invalidateThumbnail(const String &path);
  void ensureRowCapacity(uint8_t capacity);
  void layoutPaging();
  uint16_t firstEntryForPage(uint16_t page) const;
  uint16_t entryCapacityForPage(uint16_t page) const;
  void updatePageCount();
  bool ensureListing(const String &path);
  DirectoryCache *findCached(const String &path);
  DirectoryCache *allocateCacheSlot();
  void releaseCache(DirectoryCache &cache);
  void clearCache();
  bool loadDirectory(DirectoryCache &cache, const String &path);
  void sortListing();
  static int compareEntries(const Entry &left, const Entry &right);
  static int naturalCompare(const String &left, const String &right);
  static char folded(char value);
  static void quickSort(Entry *entries, int32_t left, int32_t right);
  Entry *entryAt(uint16_t index);
  const Entry *entryAt(uint16_t index) const;
  bool isHiddenEntry(const String &path) const;
  void openDirectory(const String &path);
  void openParentDirectory();
  String parentPath() const;
  String fullPathFor(const Entry &entry) const;
  String displayName(const String &path) const;
  void openEntry(uint16_t index);
  void showMenu(uint16_t index);
  void closeMenu();
  void startRename();
  void applyRename(const String &newName);
  bool validFileName(const String &name) const;
  void beginDeleteHold();
  void updateDeleteHold(uint32_t nowMs);
  void deleteSelectedFile();
  void removeEntryFromListing(uint16_t index);
  void showMessage(const String &message, uint32_t durationMs = 1800);
  void handleSdCardEvent(SdCardEvent event);
};
