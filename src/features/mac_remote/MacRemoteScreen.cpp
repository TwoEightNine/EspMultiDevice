#include "MacRemoteScreen.h"

#include "../../app/Navigator.h"
#include "../../ui/UiKit.h"
#include "../../ui/UiText.h"

namespace {
constexpr int16_t StatusTop = UiKit::AppBarHeight;
constexpr int16_t StatusHeight = 36;
constexpr int16_t CellGap = 5;
constexpr uint16_t ActiveCellColor = 0x18e3;
constexpr uint16_t PressedCellColor = 0x049f;
constexpr uint16_t DisabledCellColor = 0x2945;
constexpr uint32_t MessageDurationMs = 1500;
}

MacRemoteScreen::MacRemoteScreen(Navigator &navigator,
                                 TouchController &touch,
                                 DisplayController &display,
                                 MacRemoteController &controller)
  : Screen(navigator, touch, display),
    _controller(controller),
    _lastState(MacRemoteState::NotStarted),
    _lastTouchDown(false),
    _disconnected(false),
    _lastBusy(false),
    _pressedAction(NoAction),
    _message(),
    _messageUntilMs(0) {
  _actions[0] = {"YouTube", MacRemoteAction::YouTube, {0, 0, 0, 0}};
  _actions[1] = {"Screenshot", MacRemoteAction::Screenshot, {0, 0, 0, 0}};
  _actions[2] = {"Lock", MacRemoteAction::Lock, {0, 0, 0, 0}};
  _actions[3] = {"Previous", MacRemoteAction::Previous, {0, 0, 0, 0}};
  _actions[4] = {"Play/Pause", MacRemoteAction::PlayPause, {0, 0, 0, 0}};
  _actions[5] = {"Next", MacRemoteAction::Next, {0, 0, 0, 0}};
  _actions[6] = {"Mute", MacRemoteAction::Mute, {0, 0, 0, 0}};
  _actions[7] = {"Volume Down", MacRemoteAction::VolumeDown, {0, 0, 0, 0}};
  _actions[8] = {"Volume Up", MacRemoteAction::VolumeUp, {0, 0, 0, 0}};
}

void MacRemoteScreen::onEnter() {
  _lastTouchDown = false;
  _pressedAction = NoAction;
  _message = "";
  _messageUntilMs = 0;
  _controller.begin();
  _lastState = _controller.state();
  _lastBusy = _controller.busy();
  _disconnected = _controller.hasConnected() &&
                  _lastState == MacRemoteState::WaitingForHost;
  draw();
}

void MacRemoteScreen::onExit() {
  _controller.releaseAll();
  _pressedAction = NoAction;
  _lastTouchDown = false;
}

void MacRemoteScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    if (_pressedAction != NoAction) {
      const uint8_t released = _pressedAction;
      _pressedAction = NoAction;
      drawAction(released);
    }
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

  if (UiKit::navigationIconContains(point.x, point.y)) {
    _navigator.pop();
    return;
  }
  if (_controller.state() != MacRemoteState::Connected ||
      _controller.busy()) return;

  for (uint8_t i = 0; i < ActionCount; ++i) {
    if (!_actions[i].bounds.contains(point.x, point.y)) continue;
    _pressedAction = i;
    drawAction(i);
    if (!_controller.send(_actions[i].action)) {
      _message = F("Action could not be sent");
      _messageUntilMs = millis() + MessageDurationMs;
      drawStatus();
    } else if (_actions[i].action == MacRemoteAction::YouTube) {
      _message = F("Opening YouTube...");
      _messageUntilMs = millis() + MessageDurationMs;
      _lastBusy = true;
      drawStatus();
      drawActions();
    }
    return;
  }
}

void MacRemoteScreen::tick(uint32_t nowMs) {
  _controller.tick(nowMs);
  const MacRemoteState current = _controller.state();
  if (current != _lastState) {
    if (_lastState == MacRemoteState::Connected &&
        current == MacRemoteState::WaitingForHost) {
      _disconnected = true;
    } else if (current == MacRemoteState::Connected) {
      _disconnected = false;
      _message = "";
    }
    _lastState = current;
    drawStatus();
    drawActions();
  }
  const bool busy = _controller.busy();
  if (busy != _lastBusy) {
    _lastBusy = busy;
    drawActions();
  }
  clearMessageIfExpired(nowMs);
}

void MacRemoteScreen::layoutActions() {
  const int16_t width = _display.width();
  const int16_t gridTop = StatusTop + StatusHeight;
  const int16_t gridHeight = _display.height() - gridTop;
  const int16_t columnWidth = width / ColumnCount;
  const int16_t rowHeight = gridHeight / 3;

  for (uint8_t i = 0; i < ActionCount; ++i) {
    const uint8_t column = i % ColumnCount;
    const uint8_t row = i / ColumnCount;
    const int16_t cellLeft = column * columnWidth;
    const int16_t cellRight = column == ColumnCount - 1
                                ? width : (column + 1) * columnWidth;
    const int16_t cellTop = gridTop + row * rowHeight;
    const int16_t cellBottom = row == 2
                                ? _display.height() : gridTop + (row + 1) * rowHeight;
    _actions[i].bounds = {
      static_cast<int16_t>(cellLeft + CellGap),
      static_cast<int16_t>(cellTop + CellGap),
      static_cast<int16_t>(cellRight - cellLeft - CellGap * 2),
      static_cast<int16_t>(cellBottom - cellTop - CellGap * 2)
    };
  }
}

void MacRemoteScreen::draw() {
  if (!_display.isReady()) return;
  layoutActions();
  TFT_eSPI &tft = _display.tft();
  tft.fillScreen(TFT_BLACK);
  UiKit::drawTopAppBar(tft, _display.width(), F("Mac Remote"));
  drawStatus();
  drawActions();
}

void MacRemoteScreen::drawStatus() {
  if (!_display.isReady()) return;
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, StatusTop, _display.width(), StatusHeight, TFT_BLACK);

  String status;
  uint16_t color = TFT_LIGHTGREY;
  if (_message.length()) {
    status = _message;
    color = TFT_ORANGE;
  } else {
    switch (_controller.state()) {
      case MacRemoteState::NotStarted:
        status = F("Starting USB...");
        break;
      case MacRemoteState::WaitingForHost:
        status = _disconnected ? F("Mac disconnected")
                               : F("Connect native USB to your Mac");
        color = _disconnected ? TFT_ORANGE : TFT_CYAN;
        break;
      case MacRemoteState::Connected:
        status = F("Connected to Mac");
        color = TFT_GREEN;
        break;
      case MacRemoteState::Suspended:
        status = F("Mac suspended");
        color = TFT_ORANGE;
        break;
      case MacRemoteState::Error:
        status = F("USB failed; restart the device");
        color = TFT_RED;
        break;
    }
  }

  UiText::draw(tft, UiKit::ellipsize(status, _display.width() - 24,
                                     UiText::Size::Compact),
               _display.width() / 2, StatusTop + StatusHeight / 2,
               color, TFT_BLACK, UiText::Size::Compact, MC_DATUM);
}

void MacRemoteScreen::drawActions() {
  for (uint8_t i = 0; i < ActionCount; ++i) drawAction(i);
}

void MacRemoteScreen::drawAction(uint8_t index) {
  if (!_display.isReady() || index >= ActionCount) return;
  const bool enabled = _controller.state() == MacRemoteState::Connected &&
                       !_controller.busy();
  const ActionCell &cell = _actions[index];
  const bool pressed = enabled && _pressedAction == index;
  const uint16_t fill = !enabled ? DisabledCellColor
                                 : (pressed ? PressedCellColor : ActiveCellColor);
  const uint16_t foreground = enabled ? TFT_WHITE : TFT_DARKGREY;
  TFT_eSPI &tft = _display.tft();

  tft.fillRect(cell.bounds.x - 1, cell.bounds.y - 1,
               cell.bounds.w + 2, cell.bounds.h + 2, TFT_BLACK);
  tft.fillSmoothRoundRect(cell.bounds.x, cell.bounds.y,
                          cell.bounds.w, cell.bounds.h, 8,
                          fill, TFT_BLACK);
  const int16_t centerX = cell.bounds.x + cell.bounds.w / 2;
  const int16_t iconCenterY = cell.bounds.y + 42;
  UiKit::drawRemoteActionIcon(
    tft, centerX, iconCenterY,
    static_cast<UiKit::RemoteActionIcon>(index), foreground, fill
  );
  UiText::draw(tft, UiKit::ellipsize(String(cell.label), cell.bounds.w - 10,
                                     UiText::Size::Content),
               centerX, cell.bounds.y + cell.bounds.h - 22,
               foreground, fill, UiText::Size::Content, MC_DATUM);
}

void MacRemoteScreen::clearMessageIfExpired(uint32_t nowMs) {
  if (!_message.length() ||
      static_cast<int32_t>(nowMs - _messageUntilMs) < 0) return;
  _message = "";
  drawStatus();
}
