#include "FilesScreen.h"

#include "Navigator.h"

#include <FS.h>
#include <SD.h>

namespace {
const uint16_t FolderColor = TFT_YELLOW;
const uint16_t FileColor = TFT_WHITE;
const uint16_t ControlColor = TFT_LIGHTGREY;
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
    _rowCount(0) {
}

void FilesScreen::onEnter() {
  _currentPath = "/";
  _page = 0;
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

  if (point.x >= 10 && point.x < 90 && point.y >= 10 && point.y < 44) {
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
      } else if (isBmpFile(name)) {
        _navigator.push(ScreenId::ImageViewer, ScreenData::imagePath(nextPath));
      }
    }
    return;
  }
}

bool FilesScreen::isBmpFile(const String &name) const {
  if (name.length() < 4) return false;
  String suffix = name.substring(name.length() - 4);
  suffix.toLowerCase();
  return suffix == F(".bmp");
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
  TFT_eSPI &tft = _display.tft();
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawLine(18, 27, 34, 19, TFT_WHITE);
  tft.drawLine(18, 27, 34, 35, TFT_WHITE);
  tft.drawLine(18, 27, 48, 27, TFT_WHITE);
  tft.drawString(F("Files"), 108, 16, 4);
}

void FilesScreen::drawPathHeader() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(16, 55, _display.width() - 32, 25, TFT_DARKGREY);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  tft.drawString(_currentPath, 24, 67, 2);
  tft.setTextDatum(TL_DATUM);
}

void FilesScreen::drawListing() {
  refreshListingInfo();
  _rowCount = 0;

  if (_currentPath != "/" && _page == 0) {
    _rows[_rowCount] = { RowTop + _rowCount * RowHeight, RowHeight, RowKindEntry, 0xffff };
    TFT_eSPI &tft = _display.tft();
    tft.fillRect(18, RowTop + 8, 14, 14, FolderColor);
    // Replace the rectangle with the final parent bitmap when available:
    // tft.pushImage(18, RowTop + 5, 20, 20, parentFolderBitmap);
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(F(".."), 44, RowTop + RowHeight / 2, 2);
    tft.setTextDatum(TL_DATUM);
    _rowCount++;
  }

  if (_page > 0) {
    _rows[_rowCount] = { RowTop + _rowCount * RowHeight, RowHeight, RowKindPrevious, 0 };
    _rowCount++;
  }

  uint16_t firstEntry = _page * PageSize;
  uint16_t displayed = 0;
  uint16_t index = firstEntry;
  while (displayed < PageSize && index < _entryCount) {
    String name;
    bool isDirectory = false;
    if (entryAt(index, name, isDirectory)) {
      int16_t y = RowTop + _rowCount * RowHeight;
      TFT_eSPI &tft = _display.tft();
      uint16_t color = isDirectory ? FolderColor : FileColor;
      tft.fillRect(18, y + 8, 14, 14, color);
      // Replace the rectangle with the final bitmap when available:
      // tft.pushImage(18, y + 5, 20, 20, isDirectory ? folderBitmap : fileBitmap);
      tft.setTextDatum(ML_DATUM);
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString(truncateLabel(displayName(name), _display.width() - 58), 44, y + RowHeight / 2, 2);
      tft.setTextDatum(TL_DATUM);
      _rows[_rowCount] = { y, RowHeight, RowKindEntry, index };
      _rowCount++;
      displayed++;
    }
    index++;
  }

  if (_page + 1 < _pageCount) {
    int16_t y = RowTop + _rowCount * RowHeight;
    _rows[_rowCount] = { y, RowHeight, RowKindNext, 0 };
    _rowCount++;
    TFT_eSPI &tft = _display.tft();
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(ControlColor, TFT_BLACK);
    tft.drawString(F("Next page"), 44, y + RowHeight / 2, 2);
    tft.setTextDatum(TL_DATUM);
  }

  if (_page > 0) {
    TFT_eSPI &tft = _display.tft();
    int16_t y = RowTop;
    tft.setTextDatum(ML_DATUM);
    tft.setTextColor(ControlColor, TFT_BLACK);
    tft.drawString(F("Previous page"), 44, y + RowHeight / 2, 2);
    tft.setTextDatum(TL_DATUM);
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
  _entryCount = countEntries();
  _pageCount = (_entryCount + PageSize - 1) / PageSize;
  if (_pageCount == 0) {
    _pageCount = 1;
  }
  if (_page >= _pageCount) {
    _page = _pageCount - 1;
  }
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
    count++;
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
    if (current == index) {
      name = entry.name();
      isDirectory = entry.isDirectory();
      entry.close();
      directory.close();
      return true;
    }
    current++;
    entry.close();
    entry = directory.openNextFile();
  }
  directory.close();
  return false;
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

String FilesScreen::truncateLabel(const String &label, int16_t maxWidth) {
  TFT_eSPI &tft = _display.tft();
  if (tft.textWidth(label, 2) <= maxWidth) return label;
  String result = label;
  while (result.length() > 3 && tft.textWidth(result + "...", 2) > maxWidth) result.remove(result.length() - 1);
  return result + "...";
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
