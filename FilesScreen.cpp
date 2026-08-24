#include "FilesScreen.h"

#include "Navigator.h"
#include "UiKit.h"
#include "icons.h"

#include <FS.h>
#include <SD.h>

namespace {
const uint16_t FolderColor = TFT_YELLOW;
const uint16_t FileColor = TFT_LIGHTGREY;
const uint16_t ImageFileColor = TFT_GREEN;
const uint16_t AudioFileColor = TFT_BLUE;
}

FilesScreen::FilesScreen(Navigator &navigator,
                         TouchController &touch,
                         DisplayController &display,
                         SdCardController &sdCard)
  : Screen(navigator, touch, display),
    _sdCard(sdCard),
    _currentPath("/"),
    _page(0),
    _sdMounted(false),
    _lastTouchDown(false),
    _entryCount(0),
    _pageCount(0),
    _rows(nullptr),
    _rowCapacity(0),
    _pageSize(1),
    _rowCount(0) {
}

FilesScreen::~FilesScreen() {
  delete[] _rows;
}

void FilesScreen::onEnter() {
  _currentPath = "/";
  _page = 0;
  _lastTouchDown = false;
  _sdMounted = _sdCard.isMounted();
  draw();
}

void FilesScreen::onResume() {
  _lastTouchDown = false;
  _sdMounted = _sdCard.isMounted();
  draw();
}

void FilesScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) {
    return;
  }
  _lastTouchDown = true;

  if (UiKit::navigationIconContains(point.x, point.y)) {
    _navigator.pop();
    return;
  }

  if (!_sdMounted) {
    return;
  }

  for (uint8_t i = 0; i < _rowCount; i++) {
    if (!_rows[i].contains(point.x, point.y)) {
      continue;
    }

    if (_rows[i].kind == RowKindPrevious) {
      if (_page > 0) {
        _page--;
        draw();
      }
      return;
    }
    if (_rows[i].kind == RowKindNext) {
      if (_page + 1 < _pageCount) {
        _page++;
        draw();
      }
      return;
    }

    String name;
    bool isDirectory = false;
    if (_rows[i].entryIndex == 0xffff) {
      openParentDirectory();
    } else if (entryAt(_rows[i].entryIndex, name, isDirectory)) {
      String nextPath = name;
      if (!nextPath.startsWith("/")) {
        nextPath = _currentPath + ( _currentPath == "/" ? "" : "/" ) + nextPath;
      }
      if (isDirectory) {
        openDirectory(nextPath);
      } else if (isImageFile(name)) {
        _navigator.push(ScreenId::ImageViewer, ScreenData::imagePath(nextPath));
      } else if (isAudioFile(name)) {
        _navigator.push(ScreenId::AudioPlayer, ScreenData::audioPath(nextPath));
      }
    }
    return;
  }
}

bool FilesScreen::isImageFile(const String &name) const {
  if (name.length() < 4) return false;
  String suffix = name.substring(name.lastIndexOf('.'));
  suffix.toLowerCase();
  return suffix == F(".bmp") || suffix == F(".png") ||
         suffix == F(".jpg") || suffix == F(".jpeg") || suffix == F(".gif") ||
         suffix == F(".webp");
}

bool FilesScreen::isAudioFile(const String &name) const {
  if (name.length() < 4) return false;
  String suffix = name.substring(name.lastIndexOf('.'));
  suffix.toLowerCase();
  return suffix == F(".wav") || suffix == F(".mp3");
}

void FilesScreen::tick(uint32_t nowMs) {
  (void)nowMs;
  handleSdCardEvent(_sdCard.update());
}

void FilesScreen::draw() {
  if (!_display.isReady()) {
    return;
  }

  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  drawTopBar();
  if (!_sdMounted) {
    drawUnavailable();
    return;
  }

  drawPathHeader();
  drawListing();
}

void FilesScreen::drawTopBar() {
  UiKit::drawTopAppBar(_display.tft(), _display.width(), F("Files"));
}

void FilesScreen::drawPathHeader() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(16, 56, _display.width() - 32, 24, TFT_DARKGREY);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString(UiKit::ellipsize(tft, _currentPath, _display.width() - 48, 2), 24, 68, 2);
  tft.setTextDatum(TL_DATUM);
}

void FilesScreen::drawListing() {
  refreshListingInfo();
  _rowCount = 0;

  auto addRow = [&](int16_t y, uint8_t kind, uint16_t entryIndex) -> Row * {
    if (_rowCount >= _rowCapacity) return nullptr;
    Row &row = _rows[_rowCount++];
    row = { 0, _display.width(), y, RowHeight, kind, entryIndex };
    return &row;
  };

  if (_currentPath != "/" && _page == 0) {
    int16_t y = RowTop + _rowCount * RowHeight;
    if (!addRow(y, RowKindEntry, 0xffff)) return;
    UiKit::drawListCell(_display.tft(), _display.width(), y, F(".."), ic_dir, FolderColor);
  }

  if (_page > 0) {
    int16_t y = RowTop + _rowCount * RowHeight;
    if (!addRow(y, RowKindPrevious, 0)) return;
    UiKit::drawListCell(_display.tft(), _display.width(), y, F("Previous page"));
  }

  uint16_t firstEntry = _page * _pageSize;
  uint16_t displayed = 0;
  uint16_t index = firstEntry;
  while (displayed < _pageSize && index < _entryCount) {
    String name;
    bool isDirectory = false;
    if (entryAt(index, name, isDirectory)) {
      int16_t y = RowTop + _rowCount * RowHeight;
      if (!addRow(y, RowKindEntry, index)) return;
      if (isDirectory) {
        UiKit::drawListCell(_display.tft(), _display.width(), y,
                             displayName(name), ic_dir, FolderColor);
      } else {
        uint16_t color = isImageFile(name) ? ImageFileColor
                           : (isAudioFile(name) ? AudioFileColor : FileColor);
        UiKit::drawListCell(_display.tft(), _display.width(), y,
                             displayName(name), ic_file, color);
      }
      displayed++;
    }
    index++;
  }

  if (_page + 1 < _pageCount) {
    int16_t y = RowTop + _rowCount * RowHeight;
    if (!addRow(y, RowKindNext, 0)) return;
    UiKit::drawListCell(_display.tft(), _display.width(), y, F("Next page"));
  }
}

void FilesScreen::drawUnavailable() {
  TFT_eSPI &tft = _display.tft();
  int16_t centerX = _display.width() / 2;
  tft.fillRect(centerX - 24, 116, 48, 48, TFT_DARKGREY);
  // Replace the placeholder with the final SD error bitmap when available:
  // tft.pushImage(centerX - 24, 116, 48, 48, sdUnavailableBitmap);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(F("SD card unavailable"), centerX, 190, 2);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString(F("Insert or mount an SD card"), centerX, 216, 2);
  tft.setTextDatum(TL_DATUM);
}

void FilesScreen::refreshListingInfo() {
  uint16_t availableHeight = _display.height() > RowTop ? _display.height() - RowTop : 0;
  uint8_t rowSlots = availableHeight / RowHeight;
  // Keep room for navigation rows (parent and previous/next) on every page.
  _pageSize = rowSlots > 2 ? rowSlots - 2 : 1;
  ensureRowCapacity(rowSlots > 0 ? rowSlots : 1);
  _entryCount = countEntries();
  _pageCount = (_entryCount + _pageSize - 1) / _pageSize;
  if (_pageCount == 0) {
    _pageCount = 1;
  }
  if (_page >= _pageCount) {
    _page = _pageCount - 1;
  }
}

void FilesScreen::ensureRowCapacity(uint8_t capacity) {
  if (capacity == 0 || capacity == _rowCapacity) return;
  Row *rows = new Row[capacity];
  if (!rows) return;
  delete[] _rows;
  _rows = rows;
  _rowCapacity = capacity;
}

uint16_t FilesScreen::countEntries() const {
  File directory = SD.open(_currentPath.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return 0;
  }

  uint16_t count = 0;
  File entry = directory.openNextFile();
  while (entry) {
    String name = entry.name();
    if (!isHiddenEntry(name)) count++;
    entry.close();
    entry = directory.openNextFile();
  }
  directory.close();
  return count;
}

bool FilesScreen::entryAt(uint16_t index, String &name, bool &isDirectory) const {
  File directory = SD.open(_currentPath.c_str());
  if (!directory || !directory.isDirectory()) {
    if (directory) directory.close();
    return false;
  }

  uint16_t current = 0;
  File entry = directory.openNextFile();
  while (entry) {
    String entryName = entry.name();
    if (!isHiddenEntry(entryName)) {
      if (current == index) {
        name = entryName;
        isDirectory = entry.isDirectory();
        entry.close();
        directory.close();
        return true;
      }
      current++;
    }
    entry.close();
    entry = directory.openNextFile();
  }
  directory.close();
  return false;
}

bool FilesScreen::isHiddenEntry(const String &path) const {
  return displayName(path).startsWith(F("._"));
}

void FilesScreen::openDirectory(const String &path) {
  if (path.length() == 0) return;
  _currentPath = path;
  if (!_currentPath.startsWith("/")) _currentPath = "/" + _currentPath;
  while (_currentPath.length() > 1 && _currentPath.endsWith("/")) _currentPath.remove(_currentPath.length() - 1);
  _page = 0;
  draw();
}

void FilesScreen::openParentDirectory() {
  openDirectory(parentPath());
}

String FilesScreen::parentPath() const {
  if (_currentPath == "/") return "/";
  int16_t slash = _currentPath.lastIndexOf('/');
  return slash <= 0 ? "/" : _currentPath.substring(0, slash);
}

String FilesScreen::displayName(const String &path) const {
  int16_t slash = path.lastIndexOf('/');
  return slash < 0 ? path : path.substring(slash + 1);
}

void FilesScreen::handleSdCardEvent(SdCardEvent event) {
  if (event == SdCardEvent::None) return;
  if (event == SdCardEvent::Mounted) {
    _sdMounted = true;
    _currentPath = "/";
    _page = 0;
  } else if (event == SdCardEvent::Removed || event == SdCardEvent::MountFailed) {
    _sdMounted = false;
  }
  draw();
}
