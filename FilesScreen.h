#pragma once

#include <Arduino.h>

#include "FileFormats.h"
#include "Screen.h"
#include "SdCardController.h"
#include "SecretStorage.h"
#include "UiKit.h"

class FileThumbnailWorker;

class FilesScreen : public Screen {
public:
  FilesScreen(Navigator &navigator,
              TouchController &touch,
              DisplayController &display,
              SdCardController &sdCard,
              SecretStorage &secretStorage);
  ~FilesScreen() override;

  void onEnter() override;
  void onEnter(const ScreenData &data) override;
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
    String storageName;
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

  enum class OperationState : uint8_t {
    None,
    Deleting,
    MovePreflight,
    MoveCopying,
    MoveCommit,
    MoveRemoveSource,
    MoveCleanupStage
  };

  struct PathTask {
    String source;
    String target;
    bool directory;
    bool expanded;
  };

  SdCardController &_sdCard;
  SecretStorage &_secretStorage;
  String _currentPath;
  String _pendingRenamePath;
  String _message;
  uint32_t _messageUntilMs;
  uint16_t _page;
  bool _sdMounted;
  bool _lastTouchDown;
  bool _retainCacheWhileCovered;
  bool _secretMode;
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
  Rect _moveBounds;
  Rect _deleteBounds;
  MenuState _menuState;
  uint16_t _menuEntryIndex;
  uint32_t _deleteStartedMs;
  uint32_t _lastDeleteDrawMs;
  OperationState _operationState;
  PathTask *_tasks;
  uint16_t _taskCount;
  uint16_t _taskCapacity;
  uint16_t _taskIndex;
  String _moveSourcePath;
  String _moveTargetPath;
  String _moveStagePath;
  String _moveStagedPath;
  String _operationFailure;
  fs::File _moveSource;
  StorageFile _moveTarget;
  uint32_t _moveBytes;
  bool _moveSourceDirectory;
  bool _moveStageCreated;
  bool _movePromoted;
  bool _moveCachesCleaned;
  uint8_t _moveBuffer[4096];

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
  bool isHiddenEntry(const String &path, const String &directory) const;
  void openDirectory(const String &path);
  void openParentDirectory();
  String parentPath() const;
  String fullPathFor(const Entry &entry) const;
  String displayName(const String &path) const;
  bool canActOnEntry(const Entry &entry) const;
  void openEntry(uint16_t index);
  void showMenu(uint16_t index);
  void closeMenu();
  void startRename();
  void applyRename(const String &newName);
  bool validFileName(const String &name) const;
  void beginDeleteHold();
  void updateDeleteHold(uint32_t nowMs);
  void startDeleteSelected();
  void beginMoveToSecret();
  void updateOperation();
  void updateRecursiveDelete();
  void updateMovePreflight();
  void updateMoveCopy();
  void updateMoveCommit();
  void finishOperation(bool success, const String &message);
  void failMove(const String &message);
  void refreshListing();
  void clearTasks();
  bool appendTask(const String &source, const String &target, bool directory, bool expanded = false);
  bool expandDeleteDirectory(const PathTask &task);
  bool removePath(const String &path, bool directory);
  bool prepareMoveTarget(const Entry &entry);
  bool removePlaintextCaches(const String &sourcePath);
  void showMessage(const String &message, uint32_t durationMs = 1800);
  void handleSdCardEvent(SdCardEvent event);
};
