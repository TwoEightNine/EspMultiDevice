#include "AppLauncherScreen.h"

#include <SD.h>

#include "../../app/Navigator.h"
#include "../../core/SpiBusLock.h"
#include "../../services/storage/SdCardController.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"

namespace {
constexpr int16_t RowTop = UiKit::AppBarHeight + 8;
constexpr int16_t FooterTop = 400;
}

AppLauncherScreen::AppLauncherScreen(Navigator &navigator, TouchController &touch,
                                     DisplayController &display, SdCardController &card)
  : Screen(navigator, touch, display), _card(card) {}

void AppLauncherScreen::onEnter() {
  _lastTouchDown = false;
  startScan();
}

void AppLauncherScreen::onExit() {
  stopScan();
  for (uint8_t index = 0; index < _count; ++index) _apps[index] = AppPackage();
  _count = 0;
  _message = String();
}
void AppLauncherScreen::onResume() { onEnter(); }

void AppLauncherScreen::startScan() {
  stopScan();
  for (uint8_t index = 0; index < _count; ++index) _apps[index] = AppPackage();
  _count = _page = _scanned = 0;
  _message = "";
  _mounted = _card.isMounted();
  if (!_mounted) { _message = "Insert an SD card"; draw(); return; }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(50))) { _message = "SD busy; reopen Apps"; draw(); return; }
  _directory = SD.open("/Apps", FILE_READ);
  const bool ready = _directory && _directory.isDirectory();
  SpiBusLock::unlock();
  if (!ready) { _message = "Copy apps into /Apps"; stopScan(); }
  else _scanning = true;
  draw();
}

void AppLauncherScreen::stopScan() {
  _scanning = false;
  if (_directory && SpiBusLock::lock(portMAX_DELAY)) {
    _directory.close();
    SpiBusLock::unlock();
  }
}

void AppLauncherScreen::scanOne() {
  if (!_scanning || !_card.isMounted()) return;
  if (_scanned >= MaxScanned || _count >= MaxApps) { stopScan(); draw(); return; }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(20))) return;
  File child = _directory.openNextFile();
  const bool exists = static_cast<bool>(child);
  const bool directory = exists && child.isDirectory();
  const String path = exists ? String(child.name()) : String();
  if (exists) child.close();
  SpiBusLock::unlock();
  if (!exists) { stopScan(); draw(); return; }
  ++_scanned;
  if (!directory) return;
  const int slash = path.lastIndexOf('/');
  const String id = path.substring(slash + 1);
  if (!AppPackage::validId(id)) return;
  _apps[_count++] = AppPackage::load(_card, id);
  draw();
}

void AppLauncherScreen::tick(uint32_t) {
  const bool mounted = _card.isMounted();
  if (mounted != _mounted) { startScan(); return; }
  scanOne();
}

void AppLauncherScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) { _lastTouchDown = false; return; }
  if (_lastTouchDown) return;
  _lastTouchDown = true;
  if (UiKit::navigationIconContains(point.x, point.y)) { _navigator.pop(); return; }
  if (point.y >= RowTop && point.y < RowTop + PageSize * UiKit::ListCellHeight) {
    const uint8_t index = _page * PageSize + (point.y - RowTop) / UiKit::ListCellHeight;
    if (index >= _count) return;
    if (!_apps[index].valid) {
      _message = _apps[index].id + ": " + _apps[index].error;
      draw();
      return;
    }
    if (!_navigator.push(ScreenId::ExternalApp, ScreenData::externalApp(_apps[index].id))) {
      _message = "Could not open app";
      draw();
    }
    return;
  }
  if (point.y >= FooterTop) {
    if (point.x < _display.width() / 2 && _page > 0) --_page;
    else if (point.x >= _display.width() / 2 && (_page + 1) * PageSize < _count) ++_page;
    else return;
    _message = "";
    draw();
  }
}

void AppLauncherScreen::draw() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Apps"));
  for (uint8_t row = 0; row < PageSize; ++row) {
    const uint8_t index = _page * PageSize + row;
    if (index >= _count) break;
    const AppPackage &app = _apps[index];
    UiKit::drawListCell(tft, _display.width(), RowTop + row * UiKit::ListCellHeight,
                        app.valid ? app.name : app.id, app.valid ? String() : String("Invalid"));
  }
  if (_count == 0 && !_scanning && _message.isEmpty()) _message = "No apps found";
  if (_scanning) UiText::draw(tft, F("Scanning..."), 16, 389, TFT_LIGHTGREY, TFT_BLACK);
  UiKit::drawFilledButton(tft, 16, FooterTop + 8, 132, 40, "Previous",
                          _page > 0 ? TFT_DARKGREY : UiKit::AppBarColor);
  UiKit::drawFilledButton(tft, 172, FooterTop + 8, 132, 40, "Next",
                          (_page + 1) * PageSize < _count ? TFT_DARKGREY : UiKit::AppBarColor);
  if (!_message.isEmpty()) UiText::draw(tft, UiText::ellipsize(_message, _display.width() - 32),
                                         16, 460, TFT_WHITE, TFT_BLACK);
}
