#include "AboutScreen.h"

#include "../../app/Navigator.h"
#include "../../core/FirmwareVersion.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"

AboutScreen::AboutScreen(Navigator &navigator,
                         TouchController &touch,
                         DisplayController &display)
  : Screen(navigator, touch, display),
    _lastTouchDown(false) {
}

void AboutScreen::onEnter() {
  _lastTouchDown = false;
  draw();
}

void AboutScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;
  if (UiKit::navigationIconContains(point.x, point.y)) _navigator.pop();
}

void AboutScreen::tick(uint32_t nowMs) {
  (void)nowMs;
}

void AboutScreen::draw() {
  if (!_display.isReady()) return;

  TFT_eSPI &tft = _display.tft();
  const int16_t width = _display.width();
  const int16_t centerX = width / 2;

  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, width, F("About"));

  // The version sits at the bottom; the repository block is centered in the
  // space between the app bar and the version line.
  const int16_t versionLine = UiText::lineHeight(UiText::Size::Content);
  const int16_t versionY = _display.height() - UiKit::ContentPadding - versionLine;
  const int16_t titleLine = UiText::lineHeight(UiText::Size::Title);
  const int16_t blockHeight = titleLine + LogoGap + UiKit::LogoHeight + LogoGap + titleLine;
  const int16_t blockY = UiKit::AppBarHeight +
                         (versionY - UiKit::AppBarHeight - blockHeight) / 2;
  const int16_t logoY = blockY + titleLine + LogoGap;

  UiText::draw(tft, F("github.com/"), centerX, blockY, TFT_WHITE, TFT_BLACK,
               UiText::Size::Title, TC_DATUM);
  UiKit::drawLogo(tft, static_cast<int16_t>((width - UiKit::LogoWidth) / 2), logoY,
                  TFT_BLACK);
  UiText::draw(tft, F("/EspMultiDevice"), centerX, logoY + UiKit::LogoHeight + LogoGap,
               TFT_WHITE, TFT_BLACK, UiText::Size::Title, TC_DATUM);

  String version = F("Version: ");
  version += FirmwareVersion::Text;
  UiText::draw(tft, version, centerX, versionY, TFT_WHITE, TFT_BLACK,
               UiText::Size::Content, TC_DATUM);
}
