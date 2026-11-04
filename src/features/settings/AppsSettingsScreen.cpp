#include "AppsSettingsScreen.h"

#include <strings.h>

#include "../../app/Navigator.h"
#include "../../services/apps/AppSettings.h"
#include "../../services/storage/SdCardController.h"
#include "../../ui/UiText.h"

AppsSettingsScreen::AppsSettingsScreen(Navigator &navigator, TouchController &touch,
                                       DisplayController &display, SdCardController &card,
                                       AppSettings &settings)
  : Screen(navigator, touch, display), _card(card), _settings(settings),
    _loader(card, settings, AppCatalogLoader::Mode::All) {}

void AppsSettingsScreen::onEnter() {
  _lastTouchDown = false;
  _page = 0;
  _error = String();
  requestScan();
  draw();
}

void AppsSettingsScreen::onResume() {
  _lastTouchDown = true;
  draw();
}

bool AppsSettingsScreen::releaseResources() {
  _loader.stop();
  if (!_loader.releaseResources()) return false;
  AppCatalogLoader::clearCatalog(_catalog);
  _loaded = false;
  return true;
}

void AppsSettingsScreen::requestScan() {
  AppCatalogLoader::clearCatalog(_catalog);
  _loaded = false;
  if (_card.isMounted()) _loader.requestScan();
  else _loader.cancel();
}

uint8_t AppsSettingsScreen::pageCount() const {
  return _catalog.count ? (_catalog.count + RowsPerPage - 1) / RowsPerPage : 1;
}

void AppsSettingsScreen::sortCatalog() {
  for (uint8_t i = 0; i < _catalog.count; ++i) _order[i] = i;
  for (uint8_t i = 1; i < _catalog.count; ++i) {
    const uint8_t value = _order[i];
    uint8_t position = i;
    while (position > 0 &&
           strcasecmp(_catalog.entries[_order[position - 1]].name,
                      _catalog.entries[value].name) > 0) {
      _order[position] = _order[position - 1];
      --position;
    }
    _order[position] = value;
  }
}

void AppsSettingsScreen::tick(uint32_t nowMs) {
  (void)nowMs;
  const SdCardEvent event = _card.takeEvent();
  if (event == SdCardEvent::Mounted || event == SdCardEvent::Removed ||
      event == SdCardEvent::MountFailed) {
    _page = 0;
    requestScan();
    draw();
  }
  if (_card.isMounted() && _loader.takeCatalog(_catalog, _catalogRevision)) {
    _loaded = true;
    sortCatalog();
    if (_page >= pageCount()) _page = pageCount() - 1;
    draw();
  }
}

void AppsSettingsScreen::handleTouch(const TouchPoint &point) {
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
  if (!_loaded || point.x < 0 || point.x >= _display.width()) return;

  const int16_t footerTop = _display.height() - FooterHeight;
  if (pageCount() > 1 && point.y >= footerTop) {
    const int16_t third = _display.width() / 3;
    if (point.x < third && _page > 0) {
      --_page;
      draw();
    } else if (point.x >= third * 2 && _page + 1 < pageCount()) {
      ++_page;
      draw();
    }
    return;
  }

  if (point.y < RowTop || point.y >= RowTop + RowsPerPage * UiKit::ListCellHeight) return;
  const uint8_t slot = (point.y - RowTop) / UiKit::ListCellHeight;
  const uint16_t index = _page * RowsPerPage + slot;
  if (index >= _catalog.count) return;
  AppCatalogLoader::Entry &entry = _catalog.entries[_order[index]];
  const bool enabled = !entry.enabled;
  const String previousError = _error;
  if (_settings.setEnabled(entry.id, enabled)) {
    entry.enabled = enabled;
    _error = String();
  } else {
    _error = F("Setting could not be saved");
  }
  if (_error != previousError) draw();
  else drawRow(slot);
}

void AppsSettingsScreen::draw() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Apps"));
  if (!_card.isMounted()) {
    drawMessage(F("SD card unavailable"));
    return;
  }
  if (!_loaded) {
    drawMessage(F("Loading apps..."));
    return;
  }
  if (_catalog.count == 0) {
    drawMessage(F("No apps in /Apps"));
    return;
  }
  for (uint8_t slot = 0; slot < RowsPerPage; ++slot) drawRow(slot);
  if (pageCount() > 1) drawFooter();
  if (_error.length()) {
    const int16_t y = pageCount() > 1 ? _display.height() - FooterHeight - 14
                                      : _display.height() - 20;
    UiText::draw(tft, _error, _display.width() / 2, y, TFT_RED, TFT_BLACK,
                 UiText::Size::Compact, MC_DATUM);
  }
}

void AppsSettingsScreen::drawRow(uint8_t slot) {
  const uint16_t index = _page * RowsPerPage + slot;
  if (index >= _catalog.count || !_display.isReady()) return;
  const AppCatalogLoader::Entry &entry = _catalog.entries[_order[index]];
  UiKit::drawToggleListCell(_display.tft(), _display.width(),
                            RowTop + slot * UiKit::ListCellHeight, entry.name,
                            entry.enabled, entry.showInHome ? String() : String(F("Not in Home")));
}

void AppsSettingsScreen::drawFooter() {
  TFT_eSPI &tft = _display.tft();
  const int16_t top = _display.height() - FooterHeight;
  const int16_t third = _display.width() / 3;
  tft.fillRect(0, top, _display.width(), FooterHeight, FooterColor);
  const int16_t centerY = top + FooterHeight / 2;
  UiKit::drawPageArrow(tft, third / 2, centerY, true, false,
                       _page > 0 ? TFT_WHITE : 0x8410, FooterColor);
  UiText::draw(tft, String(_page + 1) + F(" / ") + String(pageCount()),
               _display.width() / 2, centerY, TFT_WHITE, FooterColor,
               UiText::Size::Compact, MC_DATUM);
  UiKit::drawPageArrow(tft, third * 2 + (_display.width() - third * 2) / 2, centerY, true, true,
                       _page + 1 < pageCount() ? TFT_WHITE : 0x8410, FooterColor);
}

void AppsSettingsScreen::drawMessage(const String &text) {
  UiText::draw(_display.tft(), text, _display.width() / 2, _display.height() / 2,
               TFT_LIGHTGREY, TFT_BLACK, UiText::Size::Content, MC_DATUM);
}
