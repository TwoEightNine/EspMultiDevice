#include "ImageViewerScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"
#include "../files/FilesScreen.h"

ImageViewerScreen::ImageViewerScreen(Navigator &navigator,
                                     TouchController &touch,
                                     DisplayController &display,
                                     ImageView &view)
  : Screen(navigator, touch, display),
    _view(view),
    _previousImagePath(),
    _nextImagePath(),
    _lastTouchDown(false) {
}

void ImageViewerScreen::onEnter(const ScreenData &data) {
  _lastTouchDown = false;
  _previousImagePath = data.previousImagePath;
  _nextImagePath = data.nextImagePath;
  _view.setChromePainter(paintChrome, this);
  _view.open(data.kind == ScreenDataKind::ImagePath ? data.text : String(),
             data.fileAccess == FileAccessMode::Secret, !data.imageFullscreen);
}

void ImageViewerScreen::onExit() {
  _view.close();
  _view.setChromePainter(nullptr, nullptr);
  _previousImagePath = "";
  _nextImagePath = "";
}

void ImageViewerScreen::tick(uint32_t nowMs) {
  _view.tick(nowMs);
}

void ImageViewerScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;
  const bool informative = _view.chromeVisible();
  if (informative && point.x < CloseTapSize && point.y < CloseTapSize) {
    _navigator.pop();
    return;
  }

  // The app-bar action deliberately wins over the otherwise overlapping
  // right-edge next-image target.
  if (informative && _view.zoomAvailable() && point.y < UiKit::AppBarHeight &&
      point.x >= _display.width() - UiKit::AppBarHeight) {
    _view.setCropZoom(!_view.cropZoom());
    return;
  }

  String target;
  const bool isAppBar = informative && point.y < UiKit::AppBarHeight;
  if (!isAppBar && point.x < EdgeTapWidth) {
    target = _previousImagePath;
  } else if (!isAppBar && point.x >= _display.width() - EdgeTapWidth) {
    target = _nextImagePath;
  }
  if (target.length() > 0) {
    FilesScreen *files = static_cast<FilesScreen *>(_navigator.screenFor(ScreenId::Files));
    ScreenData data;
    if (files && files->imageViewerDataFor(target, data)) {
      data.imageFullscreen = !informative;
      _navigator.replace(ScreenId::ImageViewer, data);
    }
    return;
  }

  if (_view.ready() && !isAppBar && point.x >= EdgeTapWidth &&
      point.x < _display.width() - EdgeTapWidth) _view.setChromeVisible(!informative);
}

void ImageViewerScreen::paintChrome(void *context) {
  static_cast<ImageViewerScreen *>(context)->drawChrome();
}

void ImageViewerScreen::drawChrome() {
  UiKit::drawTopAppBar(_display.tft(), _display.width(), _view.filename(), UiKit::NavigationIcon::Close,
                       _view.zoomAvailable() ? (_view.cropZoom() ? UiKit::AppBarAction::ZoomOut
                                                                  : UiKit::AppBarAction::ZoomIn)
                                             : UiKit::AppBarAction::None);
}
