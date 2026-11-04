#pragma once

#include "../../core/Screen.h"
#include "../../features/apps/AppCatalogLoader.h"
#include "../../ui/UiKit.h"

class AppSettings;
class SdCardController;

// Settings → Apps: every valid SD app package with its per-device enabled
// toggle. Apps whose manifest hides them from Home are listed too, since
// other apps can still open them.
class AppsSettingsScreen : public Screen {
public:
  AppsSettingsScreen(Navigator &navigator, TouchController &touch,
                     DisplayController &display, SdCardController &card,
                     AppSettings &settings);

  void onEnter() override;
  void onResume() override;
  bool releaseResources() override;
  void handleTouch(const TouchPoint &point) override;
  void tick(uint32_t nowMs) override;

private:
  static constexpr int16_t RowTop = UiKit::AppBarHeight + 8;
  static constexpr int16_t FooterHeight = 42;
  static constexpr uint8_t RowsPerPage = 9;
  static constexpr uint16_t FooterColor = 0x2965;

  SdCardController &_card;
  AppSettings &_settings;
  AppCatalogLoader _loader;
  AppCatalogLoader::Catalog _catalog;
  uint32_t _catalogRevision = 0;
  uint8_t _order[AppCatalogLoader::MaxApps] = {};
  bool _loaded = false;
  bool _lastTouchDown = false;
  uint8_t _page = 0;
  String _error;

  uint8_t pageCount() const;
  void requestScan();
  void sortCatalog();
  void draw();
  void drawRow(uint8_t slot);
  void drawFooter();
  void drawMessage(const String &text);
};
