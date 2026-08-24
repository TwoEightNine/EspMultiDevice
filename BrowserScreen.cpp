#include "BrowserScreen.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "Navigator.h"
#include "SdCardController.h"
#include "SpiBusLock.h"
#include "UiKit.h"
#include "WiFiController.h"
#include "icons.h"

namespace {
const int16_t HeaderHeight = UiKit::AppBarHeight;
const int16_t ControlsHeight = 42;
const int16_t TextRowHeight = 22;
const int16_t ImageRowHeight = TextRowHeight;
const int16_t DocumentPadding = 7;
const int16_t DocumentSidePadding = 8;
const int16_t StartOpenButtonTop = HeaderHeight + 8;
const int16_t StartOpenButtonHeight = 32;
const int16_t RecentListTop = StartOpenButtonTop + StartOpenButtonHeight + 24;
const uint16_t DefaultTextColor = TFT_WHITE;
const uint16_t ImagePlaceholderColor = TFT_DARKGREY;
const uint16_t LinkImageColor = TFT_BLUE;
const char *StatePath = "/.system/browser_state.txt";
const char *StateTempPath = "/.system/browser_state.tmp";
const char *DownloadDirectory = "/.system/browser_media";

String trimCopy(const String &value) {
  String result = value;
  result.trim();
  return result;
}

bool startsWithIgnoreCase(const String &value, const char *prefix) {
  size_t length = strlen(prefix);
  return value.length() >= length && value.substring(0, length).equalsIgnoreCase(prefix);
}

void drawFooterArrow(TFT_eSPI &tft, int16_t centerX, int16_t centerY,
                     bool vertical, bool forward, uint16_t color) {
  const int16_t shaft = 10;
  const int16_t wing = 7;
  for (int8_t offset = -1; offset <= 1; offset++) {
    if (vertical) {
      int16_t pointY = centerY + (forward ? wing : -wing);
      int16_t tailY = centerY - (forward ? wing : -wing);
      tft.drawLine(centerX + offset, tailY, centerX + offset, pointY, color);
      tft.drawLine(centerX + offset, pointY, centerX - wing + offset, pointY - (forward ? wing : -wing), color);
      tft.drawLine(centerX + offset, pointY, centerX + wing + offset, pointY - (forward ? wing : -wing), color);
    } else {
      int16_t pointX = centerX + (forward ? wing : -wing);
      int16_t tailX = centerX - shaft;
      if (!forward) tailX = centerX + shaft;
      tft.drawLine(tailX, centerY + offset, pointX, centerY + offset, color);
      tft.drawLine(pointX, centerY + offset, pointX - (forward ? wing : -wing), centerY - wing + offset, color);
      tft.drawLine(pointX, centerY + offset, pointX - (forward ? wing : -wing), centerY + wing + offset, color);
    }
  }
}

void drawFooterHome(TFT_eSPI &tft, int16_t centerX, int16_t centerY, uint16_t color) {
  const int16_t left = centerX - 10;
  const int16_t right = centerX + 10;
  const int16_t roof = centerY - 9;
  const int16_t base = centerY + 10;
  for (int8_t offset = -1; offset <= 1; offset++) {
    tft.drawLine(left + offset, centerY - 1, centerX + offset, roof, color);
    tft.drawLine(centerX + offset, roof, right + offset, centerY - 1, color);
    tft.drawLine(left + offset, centerY - 1, left + offset, base, color);
    tft.drawLine(right + offset, centerY - 1, right + offset, base, color);
    tft.drawLine(left + offset, base, right + offset, base, color);
  }
  tft.fillRect(centerX - 2, centerY + 3, 5, 8, color);
}

uint64_t parseHttpDate(const String &source) {
  String value = source;
  int comma = value.indexOf(',');
  if (comma >= 0) value = value.substring(comma + 1);
  value.trim();
  value.replace("-", " ");
  int day = 0;
  int year = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  char month[4] = {};
  if (sscanf(value.c_str(), "%d %3s %d %d:%d:%d", &day, month, &year, &hour, &minute, &second) != 6 ||
      year < 1970 || day < 1 || hour > 23 || minute > 59 || second > 59) return 0;
  const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  String monthText = month;
  monthText.toLowerCase();
  int monthIndex = -1;
  for (int i = 0; i < 12; i++) {
    String candidate = String(months).substring(i * 3, i * 3 + 3);
    candidate.toLowerCase();
    if (candidate == monthText) {
      monthIndex = i;
      break;
    }
  }
  if (monthIndex < 0) return 0;
  const uint8_t monthDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto leap = [](int valueYear) { return valueYear % 4 == 0 && (valueYear % 100 != 0 || valueYear % 400 == 0); };
  int maxDay = monthDays[monthIndex] + (monthIndex == 1 && leap(year) ? 1 : 0);
  if (day > maxDay) return 0;
  uint64_t days = 0;
  for (int valueYear = 1970; valueYear < year; valueYear++) days += leap(valueYear) ? 366 : 365;
  for (int i = 0; i < monthIndex; i++) days += monthDays[i] + (i == 1 && leap(year) ? 1 : 0);
  days += day - 1;
  return days * 86400ULL + hour * 3600UL + minute * 60UL + second;
}
}

BrowserScreen::BrowserScreen(Navigator &navigator,
                             TouchController &touch,
                             DisplayController &display,
                             WiFiController &wifi,
                             SdCardController &sdCard)
  : Screen(navigator, touch, display),
    _wifi(wifi),
    _sdCard(sdCard),
    _plainClient(),
    _secureClient(),
    _linkCount(0),
    _historyCount(0),
    _recentCount(0),
    _cookieCount(0),
    _loaded(false),
    _showStart(true),
    _lastTouchDown(false),
    _page(0),
    _currentUrl(),
    _title(),
    _message(),
    _titleBuffer(),
    _rowCount(0),
    _pageCount(0),
    _currentStyle{DefaultTextColor, false, false, false, false, 0},
    _styleDepth(0),
    _pendingSpace(false),
    _renderedCharacters(0),
    _closeBounds{0, 4, UiKit::NavigationIconSize, UiKit::NavigationIconSize},
    _backBounds{0, 0, 0, 0},
    _homeBounds{0, 0, 0, 0},
    _openBounds{0, 0, 0, 0},
    _previousBounds{0, 0, 0, 0},
    _nextBounds{0, 0, 0, 0} {
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    _recentBounds[i] = {0, 0, 0, 0};
  }
}

void BrowserScreen::onEnter() {
  if (!_loaded) {
    loadState();
    _loaded = true;
  }
  _lastTouchDown = false;
  draw();
}

void BrowserScreen::onResult(const ScreenResult &result) {
  if (result.kind == ScreenResultKind::Text && result.text.length() > 0) {
    openUrl(result.text, true, true);
  }
}

void BrowserScreen::handleTouch(const TouchPoint &point) {
  if (!point.touched) {
    _lastTouchDown = false;
    return;
  }
  if (_lastTouchDown) return;
  _lastTouchDown = true;

  if (contains(_closeBounds, point.x, point.y)) {
    _navigator.pop();
    return;
  }
  if (contains(_backBounds, point.x, point.y) && _historyCount > 0) {
    goBack();
    return;
  }
  if (contains(_homeBounds, point.x, point.y)) {
    _showStart = true;
    _page = 0;
    _message = "";
    draw();
    return;
  }
  if (_showStart && contains(_openBounds, point.x, point.y)) {
    openUrlInput();
    return;
  }
  if (!_showStart && contains(_previousBounds, point.x, point.y)) {
    if (_page > 0) _page--;
    draw();
    return;
  }
  if (!_showStart && contains(_nextBounds, point.x, point.y)) {
    if (hasNextPage()) _page++;
    draw();
    return;
  }
  if (_showStart) {
    for (uint8_t i = 0; i < MaxRecentSites; i++) {
      if (contains(_recentBounds[i], point.x, point.y)) {
        openUrl(_recentSites[i].url);
        return;
      }
    }
  }
  if (!_showStart) {
    String linkUrl = documentLinkAt(point.x, point.y);
    if (linkUrl.length()) openUrl(linkUrl);
  }
}

void BrowserScreen::layout() {
  const int16_t width = _display.width();
  const int16_t height = _display.height();
  const int16_t buttonWidth = width / 4;
  _closeBounds = {0, 4, UiKit::NavigationIconSize, UiKit::NavigationIconSize};
  _backBounds = {0, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _homeBounds = {buttonWidth, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _openBounds = {70, StartOpenButtonTop, (int16_t)(width - 140), StartOpenButtonHeight};
  _previousBounds = {(int16_t)(buttonWidth * 2), (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _nextBounds = {(int16_t)(buttonWidth * 3), (int16_t)(height - ControlsHeight),
                 (int16_t)(width - buttonWidth * 3), ControlsHeight};
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    _recentBounds[i] = {0, 0, 0, 0};
  }
  const int16_t footerTop = height - ControlsHeight;
  const uint8_t capacity = footerTop > RecentListTop
    ? (uint8_t)((footerTop - RecentListTop) / UiKit::ListCellHeight) : 0;
  const uint8_t visible = _recentCount < capacity ? _recentCount : capacity;
  for (uint8_t i = 0; i < visible; i++) {
    _recentBounds[i] = {0, (int16_t)(RecentListTop + i * UiKit::ListCellHeight),
                        width, UiKit::ListCellHeight};
  }
}

void BrowserScreen::draw() {
  if (!_display.isReady()) return;
  layout();
  _display.tft().setFreeFont();
  _display.tft().fillScreen(TFT_BLACK);
  drawHeader();
  if (_showStart) drawStartPage();
  else drawDocument();
  drawControls();
}

void BrowserScreen::drawHeader() {
  String heading = _showStart ? "Browser" : (_title.length() ? _title : _currentUrl);
  UiKit::drawTopAppBar(_display.tft(), _display.width(), heading);
}

void BrowserScreen::drawStartPage() {
  TFT_eSPI &tft = _display.tft();
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.fillRoundRect(_openBounds.x, _openBounds.y, _openBounds.w, _openBounds.h, 5, TFT_BLUE);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.drawString("Open URL", _display.width() / 2, StartOpenButtonTop + StartOpenButtonHeight / 2, 2);
  tft.setTextDatum(TL_DATUM);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Recent sites", 10, RecentListTop - 18, 2);
  if (_recentCount == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("No recent sites yet", 10, RecentListTop + 10, 2);
    return;
  }
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    if (_recentBounds[i].w == 0) break;
    const RecentSite &site = _recentSites[i];
    const Rect &bounds = _recentBounds[i];
    String label = site.title.length() ? site.title : site.url;
    UiKit::drawListCell(tft, bounds.w, bounds.y, label);
  }
}

void BrowserScreen::drawDocument() {
  TFT_eSPI &tft = _display.tft();
  const int16_t textTop = HeaderHeight + DocumentPadding;
  if (_message.length() > 0) {
    tft.setTextColor(_message == "Loading..." ? TFT_CYAN : TFT_RED, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(shortText(_message, _display.width() - 18), _display.width() / 2, textTop + 20, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }
  if (_rowCount == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Page has no readable text", 10, textTop, 2);
    return;
  }
  uint16_t start = _page < _pageCount ? _pageStarts[_page] : 0;
  uint16_t end = pageEndRow(_page);
  int16_t y = textTop;
  for (uint16_t index = start; index < end; index++) {
    const VisualRow &row = _rows[index];
    if (row.image) {
      uint16_t color = row.imageLink ? LinkImageColor : ImagePlaceholderColor;
      tft.drawRect(DocumentSidePadding, y, _display.width() - DocumentSidePadding * 2, ImageRowHeight, color);
      tft.drawXBitmap(DocumentSidePadding + 2, y + 2, ic_file, 16, 16, color);
      tft.setFreeFont(&FreeSans9pt7b);
      tft.setTextColor(color, TFT_BLACK);
      tft.setTextDatum(ML_DATUM);
      String label = row.imageLabel;
      const int16_t labelWidth = _display.width() - DocumentSidePadding - 24;
      while (label.length() > 3 && tft.textWidth(label + "...") > labelWidth) label.remove(label.length() - 1);
      if (label != row.imageLabel) label += "...";
      tft.drawString(label, DocumentSidePadding + 22, y + ImageRowHeight / 2);
      tft.setTextDatum(TL_DATUM);
    } else {
      for (uint8_t runIndex = 0; runIndex < row.runCount; runIndex++) {
        const VisualRun &run = row.runs[runIndex];
        const int16_t x = DocumentSidePadding + run.x;
        if (run.image) {
          uint16_t color = run.style.linkIndex ? LinkImageColor : ImagePlaceholderColor;
          tft.drawRect(x, y, run.width, ImageRowHeight, color);
          tft.drawXBitmap(x + 2, y + 3, ic_file, 16, 16, color);
          tft.setFreeFont(&FreeSans9pt7b);
          tft.setTextColor(color, TFT_BLACK);
          tft.setTextDatum(ML_DATUM);
          tft.drawString(run.text, x + 22, y + ImageRowHeight / 2);
          tft.setTextDatum(TL_DATUM);
          continue;
        }
        applyFont(run.style);
        tft.setTextColor(run.style.color, TFT_BLACK);
        tft.setTextDatum(TL_DATUM);
        tft.drawString(run.text, x, y);
        if (run.style.underline && run.width > 0) {
          tft.drawFastHLine(x, y + TextRowHeight - 2, run.width, run.style.color);
        }
      }
    }
    y += row.image ? ImageRowHeight : TextRowHeight;
  }
  tft.setFreeFont();
}

void BrowserScreen::drawControls() {
  TFT_eSPI &tft = _display.tft();
  const Rect buttons[] = {_backBounds, _homeBounds, _previousBounds, _nextBounds};
  for (uint8_t i = 0; i < 4; i++) {
    bool active = i == 1 || (i == 0 && _historyCount > 0) || (!_showStart && ((i == 2 && _page > 0) ||
      (i == 3 && hasNextPage())));
    uint16_t color = active ? 0x2965 : TFT_DARKGREY;
    const Rect &button = buttons[i];
    tft.fillRect(button.x, button.y, button.w - 1, button.h, color);
    uint16_t iconColor = active ? TFT_WHITE : 0x8410;
    const int16_t centerX = button.x + button.w / 2;
    const int16_t centerY = button.y + button.h / 2;
    if (i == 0) drawFooterArrow(tft, centerX, centerY, false, false, iconColor);
    else if (i == 1) drawFooterHome(tft, centerX, centerY, iconColor);
    else drawFooterArrow(tft, centerX, centerY, true, i == 3, iconColor);
  }
  tft.setTextDatum(TL_DATUM);
}

bool BrowserScreen::contains(const Rect &rect, int16_t x, int16_t y) const {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

String BrowserScreen::documentLinkAt(int16_t x, int16_t y) const {
  if (_message.length() || _page >= _pageCount) return "";
  const uint16_t start = _pageStarts[_page];
  const uint16_t end = pageEndRow(_page);
  int16_t rowY = HeaderHeight + DocumentPadding;
  for (uint16_t index = start; index < end; index++) {
    const VisualRow &row = _rows[index];
    const int16_t rowHeight = row.image ? ImageRowHeight : TextRowHeight;
    if (y >= rowY && y < rowY + rowHeight) {
      if (row.image) {
        bool insideImage = x >= DocumentSidePadding && x < _display.width() - DocumentSidePadding;
        return insideImage && row.imageLinkIndex ? _linkUrls[row.imageLinkIndex - 1] : "";
      }
      for (uint8_t runIndex = 0; runIndex < row.runCount; runIndex++) {
        const VisualRun &run = row.runs[runIndex];
        if (run.style.linkIndex && x >= DocumentSidePadding + run.x &&
            x < DocumentSidePadding + run.x + run.width) {
          return _linkUrls[run.style.linkIndex - 1];
        }
      }
      return "";
    }
    rowY += rowHeight;
  }
  return "";
}

void BrowserScreen::openUrlInput() {
  _navigator.push(ScreenId::TextInput, ScreenData::textInput("Open URL", false));
}

bool BrowserScreen::openUrl(const String &value, bool addToHistory, bool addToRecent) {
  const bool hadDocument = !_showStart && _currentUrl.length() > 0;
  String url;
  String error;
  if (!normalizeUrl(value, url, error)) {
    _showStart = false;
    _message = error;
    _rowCount = 0;
    _pageCount = 0;
    draw();
    return false;
  }
  if (!_wifi.isConnected()) {
    _showStart = false;
    _message = "Connect to Wi-Fi first";
    _rowCount = 0;
    _pageCount = 0;
    draw();
    return false;
  }

  _showStart = false;
  _message = "Loading...";
  _rowCount = 0;
  _pageCount = 0;
  _title = "";
  _page = 0;
  draw();
  yield();

  String finalUrl;
  String body;
  String contentType;
  String downloadedPath;
  if (!fetch(url, finalUrl, body, contentType, downloadedPath, error)) {
    _currentUrl = url;
    _message = error;
    draw();
    return false;
  }
  if (addToHistory && hadDocument) addHistory(_currentUrl);
  _currentUrl = finalUrl;
  _message = "";
  if (downloadedPath.length()) {
    FileFormats::Format format = FileFormats::fromPath(downloadedPath);
    FileFormats::Group group = FileFormats::group(format);
    renderPlainText(group == FileFormats::Group::Audio
      ? "Downloaded audio opened in player."
      : "Downloaded image opened in viewer.", group == FileFormats::Group::Audio ? "Audio download" : "Image download");
    if (addToRecent) addRecent(_currentUrl, _title);
    saveState();
    if (group == FileFormats::Group::Image &&
        _navigator.push(ScreenId::ImageViewer, ScreenData::imagePath(downloadedPath))) return true;
    if (group == FileFormats::Group::Audio &&
        _navigator.push(ScreenId::AudioPlayer, ScreenData::audioPath(downloadedPath))) return true;
    _message = "Could not open downloaded file";
    draw();
    return true;
  }
  if (contentType.indexOf("text/html") >= 0 || contentType.indexOf("application/xhtml") >= 0 ||
      contentType.length() == 0) {
    renderDocument(body);
  } else if (contentType.indexOf("text/plain") >= 0) {
    renderPlainText(body, "Text page");
  } else {
    renderPlainText("[Unsupported content: " + (contentType.length() ? contentType : String("unknown type")) + "]",
                    "Unsupported content");
  }
  if (addToRecent) addRecent(_currentUrl, _title);
  saveState();
  draw();
  return true;
}

void BrowserScreen::goBack() {
  if (_historyCount == 0) return;
  const String target = _historyUrls[_historyCount - 1];
  if (openUrl(target, false)) _historyCount--;
}

bool BrowserScreen::normalizeUrl(const String &value, String &normalized, String &error) const {
  normalized = trimCopy(value);
  error = "";
  if (normalized.length() == 0) {
    error = "Enter a URL";
    return false;
  }
  if (normalized.indexOf("://") < 0) normalized = "https://" + normalized;
  Url url;
  if (!parseUrl(normalized, url)) {
    error = "Only valid HTTP/HTTPS URLs work";
    return false;
  }
  normalized = url.scheme + "://" + url.host;
  bool defaultPort = (url.scheme == "http" && url.port == 80) || (url.scheme == "https" && url.port == 443);
  if (!defaultPort) normalized += ":" + String(url.port);
  normalized += url.path;
  return true;
}

bool BrowserScreen::parseUrl(const String &value, Url &url) const {
  int schemeEnd = value.indexOf("://");
  if (schemeEnd <= 0) return false;
  url.scheme = lower(value.substring(0, schemeEnd));
  if (url.scheme != "http" && url.scheme != "https") return false;
  int authorityStart = schemeEnd + 3;
  int pathStart = value.indexOf('/', authorityStart);
  int queryStart = value.indexOf('?', authorityStart);
  if (pathStart < 0 || (queryStart >= 0 && queryStart < pathStart)) pathStart = queryStart;
  String authority = pathStart < 0 ? value.substring(authorityStart) : value.substring(authorityStart, pathStart);
  if (authority.length() == 0 || authority.indexOf('@') >= 0) return false;
  int colon = authority.lastIndexOf(':');
  url.port = url.scheme == "https" ? 443 : 80;
  url.host = authority;
  if (colon >= 0) {
    String portText = authority.substring(colon + 1);
    long port = portText.toInt();
    if (port <= 0 || port > 65535 || String(port) != portText) return false;
    url.port = (uint16_t)port;
    url.host = authority.substring(0, colon);
  }
  url.host.trim();
  if (url.host.length() == 0 || url.host.indexOf(' ') >= 0 || url.host.indexOf('[') >= 0) return false;
  url.host = lower(url.host);
  url.path = pathStart < 0 ? "/" : value.substring(pathStart);
  if (url.path.startsWith("?")) url.path = "/" + url.path;
  int fragment = url.path.indexOf('#');
  if (fragment >= 0) url.path.remove(fragment);
  if (url.path.length() == 0) url.path = "/";
  return true;
}

bool BrowserScreen::fetch(const String &initialUrl, String &finalUrl, String &body,
                          String &contentType, String &downloadedPath, String &error) {
  body = "";
  contentType = "";
  downloadedPath = "";
  String next = initialUrl;
  for (uint8_t redirect = 0; redirect <= MaxRedirects; redirect++) {
    Url url;
    if (!parseUrl(next, url)) {
      error = "Invalid redirect URL";
      return false;
    }
    HttpResponse response{};
    WiFiClient *client = nullptr;
    if (!requestOne(url, response, client, error)) return false;
    if (response.statusCode >= 300 && response.statusCode <= 399) {
      client->stop();
      if (response.location.length() == 0) {
        error = "Redirect has no destination";
        return false;
      }
      next = resolveUrl(url, response.location);
      continue;
    }
    if (response.statusCode == 401) {
      client->stop();
      error = "Basic authentication is unsupported";
      return false;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
      client->stop();
      error = "HTTP error " + String(response.statusCode);
      return false;
    }
    contentType = response.contentType;
    FileFormats::Format format = downloadableFormatForResponse(contentType, url);
    bool downloadable = FileFormats::isDownloadable(format);
    bool readOk = false;
    if (downloadable && response.compressed) {
      error = "Compressed file response unsupported";
    } else if (downloadable) {
      readOk = readDownloadBody(*client, response.chunked, response.contentLength, format,
                                downloadedPath, error);
    } else {
      readOk = readBody(*client, response.chunked, response.contentLength, body, error);
    }
    client->stop();
    if (!readOk) return false;
    finalUrl = next;
    return true;
  }
  error = "Too many redirects";
  return false;
}

bool BrowserScreen::requestOne(const Url &url, HttpResponse &response, WiFiClient *&client,
                               String &error) {
  _plainClient.stop();
  _secureClient.stop();
  client = &_plainClient;
  if (url.scheme == "https") {
    _secureClient.setInsecure();
    client = &_secureClient;
  }
  if (!client->connect(url.host.c_str(), url.port)) {
    error = "Connection failed";
    return false;
  }
  String hostHeader = url.host;
  bool defaultPort = (url.scheme == "http" && url.port == 80) || (url.scheme == "https" && url.port == 443);
  if (!defaultPort) hostHeader += ":" + String(url.port);
  String request = "GET " + url.path + " HTTP/1.1\r\nHost: " + hostHeader +
                   "\r\nUser-Agent: ESP-Media-Browser/1.0\r\nAccept: text/html,*/*;q=0.1\r\n"
                   "Accept-Encoding: identity\r\nConnection: close\r\n";
  String cookies = cookieHeaderFor(url);
  if (cookies.length()) request += "Cookie: " + cookies + "\r\n";
  request += "\r\n";
  if (client->print(request) != request.length()) {
    client->stop();
    error = "Request failed";
    return false;
  }

  String line;
  if (!readLine(*client, line, ConnectTimeoutMs) || !startsWithIgnoreCase(line, "HTTP/")) {
    client->stop();
    error = "Invalid HTTP response";
    return false;
  }
  int firstSpace = line.indexOf(' ');
  response.statusCode = firstSpace >= 0 ? line.substring(firstSpace + 1).toInt() : 0;
  if (response.statusCode == 0) {
    client->stop();
    client = nullptr;
    error = "Invalid HTTP status";
    return false;
  }

  response.chunked = false;
  response.compressed = false;
  response.contentLength = -1;
  response.contentType = "";
  response.location = "";
  for (uint8_t count = 0; count < 48; count++) {
    if (!readLine(*client, line, ConnectTimeoutMs)) {
      client->stop();
      error = "Response headers timed out";
      return false;
    }
    if (line.length() == 0) break;
    int colon = line.indexOf(':');
    if (colon <= 0) continue;
    String name = lower(trimCopy(line.substring(0, colon)));
    String headerValue = trimCopy(line.substring(colon + 1));
    if (name == "content-length") response.contentLength = headerValue.toInt();
    else if (name == "transfer-encoding" && lower(headerValue).indexOf("chunked") >= 0) response.chunked = true;
    else if (name == "content-encoding" && !headerValue.equalsIgnoreCase("identity")) response.compressed = true;
    else if (name == "content-type") response.contentType = lower(headerValue);
    else if (name == "location") response.location = headerValue;
    else if (name == "set-cookie") acceptCookie(headerValue, url);
  }
  return true;
}

bool BrowserScreen::readLine(WiFiClient &client, String &line, uint32_t timeoutMs) const {
  line = "";
  uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    while (client.available()) {
      int value = client.read();
      if (value < 0) continue;
      if (value == '\n') {
        if (line.endsWith("\r")) line.remove(line.length() - 1);
        return true;
      }
      if (line.length() >= 2048) return false;
      line += (char)value;
    }
    if (!client.connected() && !client.available()) return line.length() > 0;
    delay(1);
    yield();
  }
  return false;
}

int BrowserScreen::readByte(WiFiClient &client, uint32_t timeoutMs) const {
  uint32_t started = millis();
  while (millis() - started < timeoutMs) {
    if (client.available()) return client.read();
    if (!client.connected()) return -1;
    delay(1);
    yield();
  }
  return -1;
}

bool BrowserScreen::readBody(WiFiClient &client, bool chunked, int32_t contentLength,
                             String &body, String &error) const {
  body = "";
  uint32_t reserveBytes = contentLength > 0
    ? (contentLength < (int32_t)MaxBodyBytes ? (uint32_t)contentLength : MaxBodyBytes)
    : 4096;
  body.reserve(reserveBytes);
  uint32_t total = 0;
  if (chunked) {
    while (true) {
      String sizeLine;
      if (!readLine(client, sizeLine, ConnectTimeoutMs)) {
        error = "Page body timed out";
        return false;
      }
      int semicolon = sizeLine.indexOf(';');
      if (semicolon >= 0) sizeLine.remove(semicolon);
      uint32_t chunk = strtoul(sizeLine.c_str(), nullptr, 16);
      if (chunk == 0) {
        do {
          if (!readLine(client, sizeLine, ConnectTimeoutMs)) break;
        } while (sizeLine.length());
        return true;
      }
      // Do not reject a large page. Retain the leading bounded portion and
      // close the connection; old pages commonly put useful body text well
      // inside this limit.
      if (chunk > MaxBodyBytes - total) {
        uint32_t keep = MaxBodyBytes - total;
        for (uint32_t i = 0; i < keep; i++) {
          int value = readByte(client, ConnectTimeoutMs);
          if (value < 0) {
            error = "Page body timed out";
            return false;
          }
          body += (char)value;
        }
        return true;
      }
      for (uint32_t i = 0; i < chunk; i++) {
        int value = readByte(client, ConnectTimeoutMs);
        if (value < 0) {
          error = "Page body timed out";
          return false;
        }
        body += (char)value;
      }
      total += chunk;
      int cr = readByte(client, ConnectTimeoutMs);
      int lf = readByte(client, ConnectTimeoutMs);
      if (cr != '\r' || lf != '\n') {
        error = "Invalid chunked response";
        return false;
      }
    }
  }

  while (contentLength < 0 || total < (uint32_t)contentLength) {
    int value = readByte(client, ConnectTimeoutMs);
    if (value < 0) {
      if (contentLength < 0) return true;
      error = "Incomplete page body";
      return false;
    }
    if (total >= MaxBodyBytes) return true;
    body += (char)value;
    total++;
  }
  return true;
}

FileFormats::Format BrowserScreen::downloadableFormatForResponse(const String &contentType, const Url &url) const {
  String type = lower(trimCopy(contentType));
  int semicolon = type.indexOf(';');
  if (semicolon >= 0) type.remove(semicolon);
  type.trim();
  FileFormats::Format format = FileFormats::fromContentType(type);
  if (FileFormats::isDownloadable(format)) return format;
  if (type.length() == 0 || type == "application/octet-stream") {
    format = FileFormats::fromPath(url.path);
    if (FileFormats::isDownloadable(format)) return format;
  }
  return FileFormats::Format::Unknown;
}

bool BrowserScreen::isSupportedDownloadFile(const String &path, FileFormats::Format expected) const {
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) return false;
  File file = SD.open(path.c_str(), FILE_READ);
  uint8_t signature[12] = {};
  const int bytes = file ? file.read(signature, sizeof(signature)) : 0;
  if (file) file.close();
  SpiBusLock::unlock();
  return FileFormats::fromSignature(signature, bytes > 0 ? (size_t)bytes : 0) == expected;
}

bool BrowserScreen::readDownloadBody(WiFiClient &client, bool chunked, int32_t contentLength,
                                     FileFormats::Format format, String &downloadedPath, String &error) {
  downloadedPath = "";
  if (!_sdCard.isMounted()) {
    error = "SD card required for downloads";
    return false;
  }
  if (contentLength > (int32_t)MaxDownloadBytes) {
    error = "File exceeds 16 MiB limit";
    return false;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    error = "SD card is busy";
    return false;
  }
  bool ready = (SD.exists("/.system") || SD.mkdir("/.system")) &&
               (SD.exists(DownloadDirectory) || SD.mkdir(DownloadDirectory));
  if (!ready) {
    SpiBusLock::unlock();
    error = "Cannot create download cache";
    return false;
  }

  const String extension = FileFormats::extension(format);
  const uint32_t stamp = millis();
  String path;
  String tempPath;
  File file;
  for (uint16_t attempt = 0; attempt < 1000; attempt++) {
    path = String(DownloadDirectory) + "/media-" + String((unsigned long)stamp) + "-" + String(attempt) + extension;
    tempPath = path + ".tmp";
    if (SD.exists(path)) continue;
    if (SD.exists(tempPath)) SD.remove(tempPath);
    file = SD.open(tempPath.c_str(), FILE_WRITE);
    if (file) break;
  }
  SpiBusLock::unlock();
  if (!file) {
    error = "Cannot create download file";
    return false;
  }

  auto removeTemp = [&]() {
    // A failed transfer must not leave a partial file behind. Waiting here is
    // safe because every normal SD user releases the shared bus lock after
    // each individual operation.
    SpiBusLock::lock();
    if (file) file.close();
    if (tempPath.length()) SD.remove(tempPath.c_str());
    SpiBusLock::unlock();
  };
  uint32_t total = 0;
  auto writeBytes = [&](const uint8_t *bytes, uint16_t length) -> bool {
    if (length > MaxDownloadBytes - total) {
      error = "File exceeds 16 MiB limit";
      return false;
    }
    if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
      error = "SD card is busy";
      return false;
    }
    size_t written = file.write(bytes, length);
    SpiBusLock::unlock();
    if (written != length) {
      error = "File download write failed";
      return false;
    }
    total += length;
    yield();
    return true;
  };
  auto receiveBytes = [&](uint32_t length) -> bool {
    uint8_t buffer[512];
    while (length) {
      uint16_t amount = length > sizeof(buffer) ? sizeof(buffer) : (uint16_t)length;
      for (uint16_t i = 0; i < amount; i++) {
        int value = readByte(client, ConnectTimeoutMs);
        if (value < 0) {
          error = "File download timed out";
          return false;
        }
        buffer[i] = (uint8_t)value;
      }
      if (!writeBytes(buffer, amount)) return false;
      length -= amount;
    }
    return true;
  };

  bool complete = true;
  if (chunked) {
    while (complete) {
      String sizeLine;
      if (!readLine(client, sizeLine, ConnectTimeoutMs)) {
        error = "File download timed out";
        complete = false;
        break;
      }
      int semicolon = sizeLine.indexOf(';');
      if (semicolon >= 0) sizeLine.remove(semicolon);
      char *end = nullptr;
      uint32_t amount = strtoul(sizeLine.c_str(), &end, 16);
      if (sizeLine.length() == 0 || !end || *end != '\0') {
        error = "Invalid chunked download";
        complete = false;
        break;
      }
      if (amount == 0) {
        do {
          if (!readLine(client, sizeLine, ConnectTimeoutMs)) {
            error = "File download timed out";
            complete = false;
            break;
          }
        } while (sizeLine.length());
        break;
      }
      if (amount > MaxDownloadBytes - total || !receiveBytes(amount)) {
        if (error.length() == 0) error = "File exceeds 16 MiB limit";
        complete = false;
        break;
      }
      int cr = readByte(client, ConnectTimeoutMs);
      int lf = readByte(client, ConnectTimeoutMs);
      if (cr != '\r' || lf != '\n') {
        error = "Invalid chunked download";
        complete = false;
      }
    }
  } else if (contentLength >= 0) {
    complete = receiveBytes((uint32_t)contentLength);
    if (!complete && error.length() == 0) error = "Incomplete file download";
  } else {
    uint8_t buffer[512];
    uint16_t buffered = 0;
    while (true) {
      int value = readByte(client, ConnectTimeoutMs);
      if (value < 0) {
        if (!client.connected() && !client.available()) break;
        error = "File download timed out";
        complete = false;
        break;
      }
      buffer[buffered++] = (uint8_t)value;
      if (buffered == sizeof(buffer)) {
        if (!writeBytes(buffer, buffered)) {
          complete = false;
          break;
        }
        buffered = 0;
      }
    }
    if (complete && buffered && !writeBytes(buffer, buffered)) complete = false;
  }

  if (!complete) {
    removeTemp();
    return false;
  }
  if (SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    file.close();
    SpiBusLock::unlock();
  } else {
    error = "SD card is busy";
    removeTemp();
    return false;
  }
  if (!isSupportedDownloadFile(tempPath, format)) {
    error = "Unsupported file download";
    removeTemp();
    return false;
  }
  if (!SpiBusLock::lock(pdMS_TO_TICKS(100))) {
    error = "SD card is busy";
    removeTemp();
    return false;
  }
  bool committed = !SD.exists(path) && SD.rename(tempPath.c_str(), path.c_str());
  if (!committed) SD.remove(tempPath.c_str());
  SpiBusLock::unlock();
  if (!committed) {
    error = "Cannot save file download";
    return false;
  }
  downloadedPath = path;
  return true;
}

String BrowserScreen::resolveUrl(const Url &base, const String &reference) const {
  String target = trimCopy(reference);
  String root = base.scheme + "://" + base.host;
  bool defaultPort = (base.scheme == "http" && base.port == 80) || (base.scheme == "https" && base.port == 443);
  if (!defaultPort) root += ":" + String(base.port);

  int fragment = target.indexOf('#');
  if (fragment >= 0) target.remove(fragment);
  if (target.length() == 0) return root + base.path;

  if (target.startsWith("//")) return base.scheme + ":" + target;
  if (target.indexOf("://") >= 0) return target;

  int colon = target.indexOf(':');
  int separator = target.length();
  int slash = target.indexOf('/');
  int query = target.indexOf('?');
  if (slash >= 0 && slash < separator) separator = slash;
  if (query >= 0 && query < separator) separator = query;
  if (colon >= 0 && colon < separator) return target;

  if (target.startsWith("/")) return root + target;
  String path = base.path;
  int queryStart = path.indexOf('?');
  if (queryStart >= 0) path.remove(queryStart);
  if (target.startsWith("?")) return root + path + target;
  String directory = path;
  slash = directory.lastIndexOf('/');
  directory = slash >= 0 ? directory.substring(0, slash + 1) : "/";
  return root + directory + target;
}

String BrowserScreen::cookieHeaderFor(const Url &url) const {
  String result;
  for (uint8_t i = 0; i < _cookieCount; i++) {
    const Cookie &cookie = _cookies[i];
    bool hostMatches = url.host == cookie.domain ||
      (url.host.endsWith("." + cookie.domain) && cookie.domain.length() > 0);
    String cookiePath = cookie.path.length() ? cookie.path : "/";
    bool pathMatches = url.path.startsWith(cookiePath) &&
      (cookiePath.endsWith("/") || url.path.length() == cookiePath.length() ||
       url.path[cookiePath.length()] == '/');
    if (!hostMatches || !pathMatches || (cookie.secure && url.scheme != "https")) continue;
    if (result.length()) result += "; ";
    result += cookie.name + "=" + cookie.value;
  }
  return result;
}

void BrowserScreen::acceptCookie(const String &header, const Url &url) {
  int separator = header.indexOf(';');
  String first = trimCopy(separator < 0 ? header : header.substring(0, separator));
  int equals = first.indexOf('=');
  if (equals <= 0) return;
  Cookie incoming;
  incoming.name = trimCopy(first.substring(0, equals));
  incoming.value = first.substring(equals + 1);
  incoming.domain = url.host;
  incoming.path = "/";
  incoming.secure = false;
  incoming.expiresAt = 0;
  int position = separator < 0 ? header.length() : separator + 1;
  while (position < (int)header.length()) {
    int end = header.indexOf(';', position);
    String attribute = trimCopy(header.substring(position, end < 0 ? header.length() : end));
    int attributeEquals = attribute.indexOf('=');
    String name = lower(trimCopy(attributeEquals < 0 ? attribute : attribute.substring(0, attributeEquals)));
    String value = attributeEquals < 0 ? "" : trimCopy(attribute.substring(attributeEquals + 1));
    if (name == "domain" && value.length()) {
      if (value.startsWith(".")) value.remove(0, 1);
      incoming.domain = lower(value);
    } else if (name == "path" && value.startsWith("/")) {
      incoming.path = value;
    } else if (name == "secure") {
      incoming.secure = true;
    } else if (name == "max-age") {
      long seconds = value.toInt();
      if (seconds <= 0) incoming.expiresAt = 1;
      else if (nowEpoch()) incoming.expiresAt = nowEpoch() + (uint64_t)seconds;
    } else if (name == "expires" && !incoming.expiresAt) {
      incoming.expiresAt = parseHttpDate(value);
    }
    if (end < 0) break;
    position = end + 1;
  }
  if (incoming.name.length() == 0) return;
  bool domainMatches = url.host == incoming.domain || url.host.endsWith("." + incoming.domain);
  if (!domainMatches) return;
  for (uint8_t i = 0; i < _cookieCount; i++) {
    if (_cookies[i].name == incoming.name && _cookies[i].domain == incoming.domain && _cookies[i].path == incoming.path) {
      _cookies[i] = incoming;
      pruneCookies();
      return;
    }
  }
  if (_cookieCount < MaxCookies) _cookies[_cookieCount++] = incoming;
  else {
    for (uint8_t i = 1; i < MaxCookies; i++) _cookies[i - 1] = _cookies[i];
    _cookies[MaxCookies - 1] = incoming;
  }
  pruneCookies();
}

void BrowserScreen::pruneCookies() {
  uint64_t now = nowEpoch();
  for (uint8_t i = 0; i < _cookieCount;) {
    if (_cookies[i].expiresAt == 1 || (now && _cookies[i].expiresAt && _cookies[i].expiresAt <= now)) {
      for (uint8_t j = i + 1; j < _cookieCount; j++) _cookies[j - 1] = _cookies[j];
      _cookieCount--;
    } else {
      i++;
    }
  }
}

uint64_t BrowserScreen::nowEpoch() const {
  time_t value = time(nullptr);
  return value > 1577836800 ? (uint64_t)value : 0;
}

void BrowserScreen::renderDocument(const String &html) {
  resetRenderedRows();
  _titleBuffer = "";
  _title = "";
  bool inScript = false;
  bool inStyle = false;
  bool inTitle = false;
  bool sawScript = false;
  uint32_t position = 0;
  while (position < html.length()) {
    int nextTag = html.indexOf('<', position);
    String text = nextTag < 0 ? html.substring(position) : html.substring(position, nextTag);
    if (inTitle) _titleBuffer += text;
    else if (!inScript && !inStyle) appendText(text);
    if (nextTag < 0) break;
    int endTag = findTagEnd(html, nextTag + 1);
    if (endTag < 0) break;
    String inside = trimCopy(html.substring(nextTag + 1, endTag));
    bool closing = inside.startsWith("/");
    if (closing) inside.remove(0, 1);
    inside.trim();
    int space = inside.indexOf(' ');
    String tag = lower(space < 0 ? inside : inside.substring(0, space));
    if (tag.endsWith("/")) tag.remove(tag.length() - 1);
    String attributes = space < 0 ? "" : trimCopy(inside.substring(space + 1));
    bool selfClosing = inside.endsWith("/");
    if (tag.startsWith("!") || tag.startsWith("?")) {
      position = endTag + 1;
      continue;
    }
    if (tag == "script") {
      if (!closing) sawScript = true;
      inScript = !closing;
    }
    else if (tag == "style") inStyle = !closing;
    else if (tag == "title") inTitle = !closing;
    else if (!inScript && !inStyle && !inTitle) {
      if (closing) {
        if (isBlockTag(tag)) appendBreak();
        if (tag == "td" || tag == "th") _pendingSpace = true;
        popStyle(tag);
      } else if (tag == "br") {
        appendBreak();
      } else if (tag == "img") {
        String alt = decodeEntities(attributeValue(attributes, "alt"));
        appendImage(alt.length() ? alt : "image", _currentStyle.link, _currentStyle.linkIndex,
                    isFullWidthImage(attributes));
      } else if (tag == "audio" || tag == "video" || tag == "iframe" || tag == "object" ||
                 tag == "embed" || tag == "canvas") {
        appendStub(tag);
      } else if (tag == "form" || tag == "input" || tag == "button" || tag == "textarea" || tag == "select") {
        appendStub("form control");
      } else {
        if (isBlockTag(tag)) appendBreak();
        if ((tag == "td" || tag == "th") && _rowCount) _pendingSpace = true;
        if (!selfClosing) pushStyle(tag, styledForTag(tag, attributes));
      }
    }
    position = endTag + 1;
  }
  _title = decodeEntities(trimCopy(_titleBuffer));
  if (_title.length() == 0) _title = "Web page";
  if (_rowCount == 0) appendText(sawScript
    ? "(This page is generated by JavaScript. JavaScript is not supported.)"
    : "(This page has no static readable text.)");
  buildPages();
}

void BrowserScreen::renderPlainText(const String &text, const String &title) {
  resetRenderedRows();
  _title = title;
  appendText(text);
  if (_rowCount == 0) appendText("(No readable text in this response.)");
  buildPages();
}

void BrowserScreen::resetRenderedRows() {
  _rowCount = 0;
  _pageCount = 0;
  _styleDepth = 0;
  _currentStyle = {DefaultTextColor, false, false, false, false, 0};
  _linkCount = 0;
  _pendingSpace = false;
  _renderedCharacters = 0;
}

void BrowserScreen::appendText(const String &text) {
  String decoded = decodeEntities(text);
  String word;
  for (uint16_t i = 0; i <= decoded.length(); i++) {
    char value = i < decoded.length() ? decoded[i] : ' ';
    if (value != ' ' && value != '\r' && value != '\n' && value != '\t') {
      word += value;
      continue;
    }
    if (word.length()) {
      appendWord(word, _currentStyle);
      word = "";
    }
    if (value == '\n' || value == '\r') appendBreak();
    else _pendingSpace = true;
  }
}

void BrowserScreen::appendBreak() {
  _pendingSpace = false;
  if (_rowCount && !_rows[_rowCount - 1].image && _rows[_rowCount - 1].runCount) {
    _rows[_rowCount - 1].contentWidth = _display.width();
  }
}

void BrowserScreen::appendImage(const String &label, bool linked, uint8_t linkIndex, bool fullWidth) {
  TextStyle style = _currentStyle;
  style.link = linked;
  style.linkIndex = linkIndex;
  if (!fullWidth) {
    appendInlineImage(label, style);
    return;
  }
  appendBreak();
  if (_rowCount >= MaxRows) return;
  VisualRow &row = _rows[_rowCount++];
  row.image = true;
  row.imageLabel = label;
  row.imageLink = linked;
  row.imageLinkIndex = linkIndex;
  row.contentWidth = 0;
  row.runCount = 0;
}

void BrowserScreen::appendInlineImage(const String &label, const TextStyle &style) {
  const int16_t maxWidth = _display.width() - DocumentSidePadding * 2;
  String displayLabel = shortImageLabel(label, maxWidth - 26);
  int16_t width = imageWidth(displayLabel);
  beginTextRow();
  if (_rowCount == 0) return;

  VisualRow *row = &_rows[_rowCount - 1];
  if (_pendingSpace && row->contentWidth > 0) {
    const int16_t spaceWidth = textWidth(" ", style);
    if (row->contentWidth + spaceWidth + width > maxWidth || row->runCount + 2 > MaxRunsPerRow) {
      row->contentWidth = _display.width();
      beginTextRow();
      if (_rowCount == 0) return;
      row = &_rows[_rowCount - 1];
    } else {
      appendRun(" ", style);
      row = &_rows[_rowCount - 1];
    }
  }
  _pendingSpace = false;

  if ((row->contentWidth > 0 && row->contentWidth + width > maxWidth) ||
      row->runCount >= MaxRunsPerRow) {
    row->contentWidth = _display.width();
    beginTextRow();
    if (_rowCount == 0) return;
    row = &_rows[_rowCount - 1];
  }
  VisualRun &run = row->runs[row->runCount++];
  run.image = true;
  run.text = displayLabel;
  run.style = style;
  run.x = row->contentWidth;
  run.width = width;
  row->contentWidth += width;
}

void BrowserScreen::appendStub(const String &label) {
  appendBreak();
  appendText(String("(") + label + ")");
  appendBreak();
}

void BrowserScreen::appendWord(const String &word, const TextStyle &style) {
  if (_renderedCharacters >= 16384) return;
  String remaining = word;
  const int16_t maxWidth = _display.width() - DocumentSidePadding * 2;
  while (remaining.length() && _rowCount < MaxRows) {
    beginTextRow();
    VisualRow &row = _rows[_rowCount - 1];
    if (_pendingSpace && row.contentWidth > 0) {
      int16_t spaceWidth = textWidth(" ", style);
      if (row.contentWidth + spaceWidth >= maxWidth) {
        row.contentWidth = _display.width();
        beginTextRow();
      } else {
        appendRun(" ", style);
      }
    }
    _pendingSpace = false;
    VisualRow &target = _rows[_rowCount - 1];
    int16_t available = maxWidth - target.contentWidth;
    int16_t wholeWidth = textWidth(remaining, style);
    if (wholeWidth <= available) {
      appendRun(remaining, style);
      _renderedCharacters += remaining.length();
      return;
    }
    String part;
    while (part.length() < remaining.length() && textWidth(part + remaining[part.length()], style) <= available) {
      part += remaining[part.length()];
    }
    if (part.length() == 0) {
      if (target.contentWidth > 0) {
        target.contentWidth = _display.width();
        continue;
      }
      part = remaining.substring(0, 1);
    }
    appendRun(part, style);
    _renderedCharacters += part.length();
    remaining.remove(0, part.length());
    if (remaining.length()) _rows[_rowCount - 1].contentWidth = _display.width();
  }
}

void BrowserScreen::appendRun(const String &text, const TextStyle &style) {
  if (text.length() == 0 || _rowCount == 0) return;
  VisualRow &row = _rows[_rowCount - 1];
  int16_t width = textWidth(text, style);
  if (row.runCount && !row.runs[row.runCount - 1].image &&
      stylesEqual(row.runs[row.runCount - 1].style, style)) {
    VisualRun &run = row.runs[row.runCount - 1];
    run.text += text;
    run.width += width;
  } else if (row.runCount < MaxRunsPerRow) {
    VisualRun &run = row.runs[row.runCount++];
    run.image = false;
    run.text = text;
    run.style = style;
    run.x = row.contentWidth;
    run.width = width;
  } else {
    row.contentWidth = _display.width();
    beginTextRow();
    appendRun(text, style);
    return;
  }
  row.contentWidth += width;
}

void BrowserScreen::beginTextRow() {
  const int16_t maxWidth = _display.width() - DocumentSidePadding * 2;
  if (_rowCount && !_rows[_rowCount - 1].image && _rows[_rowCount - 1].contentWidth < maxWidth) return;
  if (_rowCount >= MaxRows) return;
  VisualRow &row = _rows[_rowCount++];
  row.image = false;
  row.imageLabel = "";
  row.imageLink = false;
  row.imageLinkIndex = 0;
  row.contentWidth = 0;
  row.runCount = 0;
}

void BrowserScreen::buildPages() {
  _pageCount = 0;
  if (_rowCount == 0) return;
  const int16_t bottom = _display.height() - ControlsHeight - 4;
  uint16_t first = 0;
  int16_t y = HeaderHeight + DocumentPadding;
  _pageStarts[_pageCount++] = 0;
  for (uint16_t i = 0; i < _rowCount; i++) {
    int16_t height = _rows[i].image ? ImageRowHeight : TextRowHeight;
    if (y + height > bottom && i > first) {
      if (_pageCount >= MaxPages) break;
      first = i;
      _pageStarts[_pageCount++] = i;
      y = HeaderHeight + DocumentPadding;
    }
    y += height;
  }
  if (_page >= _pageCount) _page = 0;
}

uint16_t BrowserScreen::pageEndRow(uint8_t page) const {
  if (page + 1 < _pageCount) return _pageStarts[page + 1];
  return _rowCount;
}

bool BrowserScreen::hasNextPage() const {
  return _page + 1 < _pageCount;
}

int16_t BrowserScreen::textWidth(const String &text, const TextStyle &style) const {
  applyFont(style);
  return _display.tft().textWidth(text);
}

void BrowserScreen::applyFont(const TextStyle &style) const {
  TFT_eSPI &tft = _display.tft();
  if (style.bold && style.italic) tft.setFreeFont(&FreeSansBoldOblique9pt7b);
  else if (style.bold) tft.setFreeFont(&FreeSansBold9pt7b);
  else if (style.italic) tft.setFreeFont(&FreeSansOblique9pt7b);
  else tft.setFreeFont(&FreeSans9pt7b);
}

bool BrowserScreen::stylesEqual(const TextStyle &left, const TextStyle &right) const {
  return left.color == right.color && left.bold == right.bold && left.italic == right.italic &&
         left.underline == right.underline && left.link == right.link && left.linkIndex == right.linkIndex;
}

BrowserScreen::TextStyle BrowserScreen::styledForTag(const String &tag, const String &attributes) {
  TextStyle style = _currentStyle;
  if (tag == "b" || tag == "strong") style.bold = true;
  if (tag == "i" || tag == "em") style.italic = true;
  if (tag == "a") {
    style.underline = true;
    style.link = true;
    style.linkIndex = 0;
    String href = decodeEntities(attributeValue(attributes, "href"));
    href.trim();
    if (href.length() && !href.startsWith("#")) {
      Url base;
      String resolved;
      String error;
      if (parseUrl(_currentUrl, base)) resolved = resolveUrl(base, href);
      String normalized;
      bool isHttp = startsWithIgnoreCase(resolved, "http://") || startsWithIgnoreCase(resolved, "https://");
      if (isHttp && normalizeUrl(resolved, normalized, error)) style.linkIndex = registerLink(normalized);
      else style.linkIndex = 0;
    }
  }

  String color = attributeValue(attributes, "color");
  if (color.length()) style.color = parseColor(color, style.color);

  String inlineStyle = attributeValue(attributes, "style");
  int start = 0;
  while (start < (int)inlineStyle.length()) {
    int end = inlineStyle.indexOf(';', start);
    String declaration = trimCopy(inlineStyle.substring(start, end < 0 ? inlineStyle.length() : end));
    int colon = declaration.indexOf(':');
    if (colon > 0) {
      String property = lower(trimCopy(declaration.substring(0, colon)));
      String value = lower(trimCopy(declaration.substring(colon + 1)));
      if (property == "color") style.color = parseColor(value, style.color);
      else if (property == "font-weight") style.bold = value.indexOf("bold") >= 0 || value.toInt() >= 600;
      else if (property == "font-style") style.italic = value.indexOf("italic") >= 0 || value.indexOf("oblique") >= 0;
    }
    if (end < 0) break;
    start = end + 1;
  }
  return style;
}

uint8_t BrowserScreen::registerLink(const String &url) {
  for (uint8_t index = 0; index < _linkCount; index++) {
    if (_linkUrls[index] == url) return index + 1;
  }
  if (_linkCount >= MaxLinks) return 0;
  _linkUrls[_linkCount] = url;
  _linkCount++;
  return _linkCount;
}

String BrowserScreen::attributeValue(const String &attributes, const char *name) const {
  String wanted = lower(String(name));
  uint16_t index = 0;
  while (index < attributes.length()) {
    while (index < attributes.length() && (isspace((unsigned char)attributes[index]) || attributes[index] == '/')) index++;
    uint16_t nameStart = index;
    while (index < attributes.length() && !isspace((unsigned char)attributes[index]) &&
           attributes[index] != '=' && attributes[index] != '/') index++;
    if (nameStart == index) {
      index++;
      continue;
    }
    String attributeName = lower(attributes.substring(nameStart, index));
    while (index < attributes.length() && isspace((unsigned char)attributes[index])) index++;
    if (index >= attributes.length() || attributes[index] != '=') {
      continue;
    }
    index++;
    while (index < attributes.length() && isspace((unsigned char)attributes[index])) index++;
    String value;
    if (index < attributes.length() && (attributes[index] == '\'' || attributes[index] == '\"')) {
      char quote = attributes[index++];
      uint16_t valueStart = index;
      while (index < attributes.length() && attributes[index] != quote) index++;
      value = attributes.substring(valueStart, index);
      if (index < attributes.length()) index++;
    } else {
      uint16_t valueStart = index;
      while (index < attributes.length() && !isspace((unsigned char)attributes[index])) index++;
      value = attributes.substring(valueStart, index);
    }
    if (attributeName == wanted) return value;
  }
  return "";
}

uint16_t BrowserScreen::parseColor(const String &rawValue, uint16_t fallback) const {
  String value = lower(trimCopy(rawValue));
  if (value.startsWith("#")) {
    String digits = value.substring(1);
    for (uint16_t i = 0; i < digits.length(); i++) {
      if (!isxdigit((unsigned char)digits[i])) return fallback;
    }
    char *end = nullptr;
    if (digits.length() == 3) {
      uint8_t red = strtoul(digits.substring(0, 1).c_str(), &end, 16) * 17;
      uint8_t green = strtoul(digits.substring(1, 2).c_str(), &end, 16) * 17;
      uint8_t blue = strtoul(digits.substring(2, 3).c_str(), &end, 16) * 17;
      return ((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3);
    }
    if (digits.length() == 6) {
      uint8_t red = strtoul(digits.substring(0, 2).c_str(), &end, 16);
      uint8_t green = strtoul(digits.substring(2, 4).c_str(), &end, 16);
      uint8_t blue = strtoul(digits.substring(4, 6).c_str(), &end, 16);
      return ((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3);
    }
  }
  if (value == "black") return TFT_BLACK;
  if (value == "white") return TFT_WHITE;
  if (value == "red") return TFT_RED;
  if (value == "green") return TFT_GREEN;
  if (value == "blue") return TFT_BLUE;
  if (value == "yellow") return TFT_YELLOW;
  if (value == "cyan" || value == "aqua") return TFT_CYAN;
  if (value == "magenta" || value == "fuchsia") return TFT_MAGENTA;
  if (value == "gray" || value == "grey") return TFT_LIGHTGREY;
  if (value == "darkgray" || value == "darkgrey") return TFT_DARKGREY;
  if (value == "orange") return TFT_ORANGE;
  if (value == "purple") return 0x8010;
  return fallback;
}

int BrowserScreen::findTagEnd(const String &html, int start) const {
  char quote = '\0';
  for (int index = start; index < (int)html.length(); index++) {
    char value = html[index];
    if (quote) {
      if (value == quote) quote = '\0';
    } else if (value == '\'' || value == '\"') {
      quote = value;
    } else if (value == '>') {
      return index;
    }
  }
  return -1;
}

void BrowserScreen::pushStyle(const String &tag, const TextStyle &style) {
  if (_styleDepth >= MaxStyleDepth) return;
  _styleStack[_styleDepth].tag = tag;
  _styleStack[_styleDepth].style = style;
  _styleDepth++;
  _currentStyle = style;
}

void BrowserScreen::popStyle(const String &tag) {
  for (int8_t index = _styleDepth - 1; index >= 0; index--) {
    if (_styleStack[index].tag != tag) continue;
    _styleDepth = index;
    _currentStyle = index > 0 ? _styleStack[index - 1].style :
      TextStyle{DefaultTextColor, false, false, false, false, 0};
    return;
  }
}

String BrowserScreen::decodeEntities(const String &value) const {
  String result = value;
  result.replace("&nbsp;", " ");
  result.replace("&amp;", "&");
  result.replace("&lt;", "<");
  result.replace("&gt;", ">");
  result.replace("&quot;", "\"");
  result.replace("&#39;", "'");
  result.replace("&apos;", "'");
  String decoded;
  for (uint32_t i = 0; i < result.length();) {
    if (result[i] == '&' && i + 3 < result.length() && result[i + 1] == '#') {
      int end = result.indexOf(';', i + 2);
      if (end > (int)i + 2) {
        String number = result.substring(i + 2, end);
        bool hexadecimal = number.startsWith("x") || number.startsWith("X");
        if (hexadecimal) number.remove(0, 1);
        char *numberEnd = nullptr;
        uint32_t codepoint = strtoul(number.c_str(), &numberEnd, hexadecimal ? 16 : 10);
        if (number.length() && numberEnd && *numberEnd == '\0' && codepoint <= 0x10ffffUL) {
          if (codepoint < 0x80) decoded += (char)codepoint;
          else if (codepoint < 0x800) {
            decoded += (char)(0xc0 | (codepoint >> 6));
            decoded += (char)(0x80 | (codepoint & 0x3f));
          } else if (codepoint < 0x10000) {
            decoded += (char)(0xe0 | (codepoint >> 12));
            decoded += (char)(0x80 | ((codepoint >> 6) & 0x3f));
            decoded += (char)(0x80 | (codepoint & 0x3f));
          }
          i = end + 1;
          continue;
        }
      }
    }
    decoded += result[i++];
  }
  return transliterateCyrillic(decoded);
}

String BrowserScreen::transliterateCyrillic(const String &value) const {
  // UTF-8 Cyrillic is split across several byte ranges. In particular,
  // lowercase а-п are D0 B0..BF; omitting that range was the source of the
  // question marks seen on Russian pages.
  static const char *ExtendedUpper[] = {
    "E", "Yo", "Dj", "Gj", "Ye", "Dz", "I", "Yi", "J", "Lj", "Nj", "C", "K", "I", "U", "Dzh"
  };
  static const char *Upper[] = {
    "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",
    "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Shch", "\"", "Y", "'", "E", "Yu", "Ya"
  };
  // а-п: U+0430..U+043F -> D0 B0..BF
  static const char *RussianLowerAtoP[] = {
    "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p"
  };
  // р-я: U+0440..U+044F -> D1 80..8F
  static const char *RussianLowerRtoYa[] = {
    "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "shch", "\"", "y", "'", "e", "yu", "ya"
  };
  static const char *ExtendedLower[] = {
    "e", "yo", "dj", "gj", "ye", "dz", "i", "yi", "j", "lj", "nj", "c", "k", "i", "u", "dzh"
  };
  String result;
  for (uint16_t i = 0; i < value.length(); i++) {
    uint8_t first = (uint8_t)value[i];
    if (first < 0x80) {
      result += (char)first;
      continue;
    }
    if (i + 1 >= value.length()) {
      result += '?';
      break;
    }
    uint8_t second = (uint8_t)value[++i];
    if (first == 0xd0 && second >= 0x80 && second <= 0x8f) result += ExtendedUpper[second - 0x80];
    else if (first == 0xd0 && second >= 0x90 && second <= 0xaf) result += Upper[second - 0x90];
    else if (first == 0xd0 && second >= 0xb0 && second <= 0xbf) result += RussianLowerAtoP[second - 0xb0];
    else if (first == 0xd1 && second >= 0x80 && second <= 0x8f) result += RussianLowerRtoYa[second - 0x80];
    else if (first == 0xd1 && second >= 0x90 && second <= 0x9f) result += ExtendedLower[second - 0x90];
    else if (first == 0xd2 && second == 0x90) result += "G";
    else if (first == 0xd2 && second == 0x91) result += "g";
    else result += '?';
  }
  return result;
}

String BrowserScreen::shortText(const String &value, int16_t maxWidth) const {
  if (_display.tft().textWidth(value, 2) <= maxWidth) return value;
  String result = value;
  while (result.length() > 3 && _display.tft().textWidth(result + "...", 2) > maxWidth) result.remove(result.length() - 1);
  return result + "...";
}

String BrowserScreen::shortImageLabel(const String &value, int16_t maxWidth) const {
  TextStyle style{DefaultTextColor, false, false, false, false, 0};
  if (textWidth(value, style) <= maxWidth) return value;
  String result = value;
  while (result.length() > 3 && textWidth(result + "...", style) > maxWidth) {
    result.remove(result.length() - 1);
  }
  return result.length() == value.length() ? result : result + "...";
}

String BrowserScreen::lower(const String &value) const {
  String result = value;
  result.toLowerCase();
  return result;
}

bool BrowserScreen::isBlockTag(const String &tag) const {
  return tag == "p" || tag == "div" || tag == "section" || tag == "article" || tag == "header" ||
         tag == "footer" || tag == "main" || tag == "nav" || tag == "h1" || tag == "h2" || tag == "h3" ||
         tag == "h4" || tag == "h5" || tag == "h6" || tag == "ul" || tag == "ol" || tag == "li" ||
         tag == "table" || tag == "tr" || tag == "blockquote" || tag == "hr";
}

bool BrowserScreen::isFullWidthImage(const String &attributes) const {
  String width = lower(trimCopy(attributeValue(attributes, "width")));
  if (width == "100%") return true;
  String inlineStyle = attributeValue(attributes, "style");
  int start = 0;
  while (start < (int)inlineStyle.length()) {
    int end = inlineStyle.indexOf(';', start);
    String declaration = trimCopy(inlineStyle.substring(start, end < 0 ? inlineStyle.length() : end));
    int colon = declaration.indexOf(':');
    if (colon > 0) {
      String property = lower(trimCopy(declaration.substring(0, colon)));
      String value = lower(trimCopy(declaration.substring(colon + 1)));
      if ((property == "width" && value == "100%") ||
          (property == "display" && (value == "block" || value.startsWith("block ")))) return true;
    }
    if (end < 0) break;
    start = end + 1;
  }
  return false;
}

int16_t BrowserScreen::imageWidth(const String &label) const {
  TextStyle style{DefaultTextColor, false, false, false, false, 0};
  return 26 + textWidth(label, style);
}

void BrowserScreen::addHistory(const String &url) {
  if (url.length() == 0) return;
  if (_historyCount >= MaxHistory) {
    for (uint8_t index = 1; index < MaxHistory; index++) {
      _historyUrls[index - 1] = _historyUrls[index];
    }
    _historyCount = MaxHistory - 1;
  }
  _historyUrls[_historyCount++] = url;
}

void BrowserScreen::addRecent(const String &url, const String &title) {
  uint8_t found = _recentCount;
  for (uint8_t i = 0; i < _recentCount; i++) {
    if (_recentSites[i].url == url) {
      found = i;
      break;
    }
  }
  if (found < _recentCount) {
    for (uint8_t i = found; i > 0; i--) _recentSites[i] = _recentSites[i - 1];
  } else {
    if (_recentCount < MaxRecentSites) _recentCount++;
    for (uint8_t i = _recentCount - 1; i > 0; i--) _recentSites[i] = _recentSites[i - 1];
  }
  _recentSites[0] = {url, title};
}

void BrowserScreen::loadState() {
  _recentCount = 0;
  _cookieCount = 0;
  if (!_sdCard.isMounted() || !SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
  File file = SD.open(StatePath, FILE_READ);
  if (!file) {
    SpiBusLock::unlock();
    return;
  }
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.length() < 2 || line[1] != '\t') continue;
    String fields[7];
    uint8_t fieldCount = 0;
    int start = 2;
    while (fieldCount < 7 && start <= (int)line.length()) {
      int end = line.indexOf('\t', start);
      fields[fieldCount++] = line.substring(start, end < 0 ? line.length() : end);
      if (end < 0) break;
      start = end + 1;
    }
    if (line[0] == 'R' && fieldCount >= 2 && _recentCount < MaxRecentSites) {
      _recentSites[_recentCount++] = {decodeField(fields[0]), decodeField(fields[1])};
    } else if (line[0] == 'C' && fieldCount >= 6 && _cookieCount < MaxCookies) {
      Cookie &cookie = _cookies[_cookieCount++];
      cookie.name = decodeField(fields[0]);
      cookie.value = decodeField(fields[1]);
      cookie.domain = decodeField(fields[2]);
      cookie.path = decodeField(fields[3]);
      cookie.secure = fields[4] == "1";
      cookie.expiresAt = strtoull(fields[5].c_str(), nullptr, 10);
    }
  }
  file.close();
  SpiBusLock::unlock();
  pruneCookies();
}

void BrowserScreen::saveState() {
  pruneCookies();
  if (!_sdCard.isMounted() || !SpiBusLock::lock(pdMS_TO_TICKS(100))) return;
  if (!SD.exists("/.system") && !SD.mkdir("/.system")) {
    SpiBusLock::unlock();
    return;
  }
  SD.remove(StateTempPath);
  File file = SD.open(StateTempPath, FILE_WRITE);
  if (!file) {
    SpiBusLock::unlock();
    return;
  }
  bool written = true;
  for (uint8_t i = 0; i < _recentCount; i++) {
    written = written && file.printf("R\t%s\t%s\n", encodeField(_recentSites[i].url).c_str(),
                                     encodeField(_recentSites[i].title).c_str()) > 0;
  }
  for (uint8_t i = 0; i < _cookieCount; i++) {
    const Cookie &cookie = _cookies[i];
    written = written && file.printf("C\t%s\t%s\t%s\t%s\t%d\t%llu\n",
      encodeField(cookie.name).c_str(), encodeField(cookie.value).c_str(),
      encodeField(cookie.domain).c_str(), encodeField(cookie.path).c_str(), cookie.secure ? 1 : 0,
      (unsigned long long)cookie.expiresAt) > 0;
  }
  file.close();
  if (written) {
    if (SD.exists(StatePath)) SD.remove(StatePath);
    if (!SD.rename(StateTempPath, StatePath)) SD.remove(StateTempPath);
  } else {
    SD.remove(StateTempPath);
  }
  SpiBusLock::unlock();
}

String BrowserScreen::encodeField(const String &value) const {
  const char hex[] = "0123456789ABCDEF";
  String result;
  for (uint16_t i = 0; i < value.length(); i++) {
    uint8_t valueByte = (uint8_t)value[i];
    if (isalnum(valueByte) || valueByte == '-' || valueByte == '_' || valueByte == '.' || valueByte == '/') result += (char)valueByte;
    else {
      result += '%';
      result += hex[valueByte >> 4];
      result += hex[valueByte & 15];
    }
  }
  return result;
}

String BrowserScreen::decodeField(const String &value) const {
  String result;
  for (uint16_t i = 0; i < value.length(); i++) {
    if (value[i] == '%' && i + 2 < value.length()) {
      char buffer[] = {value[i + 1], value[i + 2], '\0'};
      result += (char)strtoul(buffer, nullptr, 16);
      i += 2;
    } else {
      result += value[i];
    }
  }
  return result;
}
