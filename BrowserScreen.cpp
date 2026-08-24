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
#include "WiFiController.h"

namespace {
const int16_t HeaderHeight = 38;
const int16_t ControlsHeight = 42;
const int16_t PageLineHeight = 18;
const uint8_t LinesPerPage = 12;
const char *StatePath = "/.system/browser_state.txt";
const char *StateTempPath = "/.system/browser_state.tmp";

String trimCopy(const String &value) {
  String result = value;
  result.trim();
  return result;
}

bool startsWithIgnoreCase(const String &value, const char *prefix) {
  size_t length = strlen(prefix);
  return value.length() >= length && value.substring(0, length).equalsIgnoreCase(prefix);
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
    _recentCount(0),
    _cookieCount(0),
    _loaded(false),
    _showStart(true),
    _lastTouchDown(false),
    _page(0),
    _currentUrl(),
    _title(),
    _message(),
    _renderBuffer(),
    _titleBuffer(),
    _lineCount(0),
    _closeBounds{0, 0, 0, 0},
    _startBounds{0, 0, 0, 0},
    _newBounds{0, 0, 0, 0},
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
    openUrl(result.text);
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
  if (contains(_startBounds, point.x, point.y)) {
    _showStart = true;
    _page = 0;
    _message = "";
    draw();
    return;
  }
  if (contains(_newBounds, point.x, point.y) || (_showStart && contains(_openBounds, point.x, point.y))) {
    openUrlInput();
    return;
  }
  if (!_showStart && contains(_previousBounds, point.x, point.y)) {
    if (_page > 0) _page--;
    draw();
    return;
  }
  if (!_showStart && contains(_nextBounds, point.x, point.y)) {
    uint16_t pageCount = (_lineCount + LinesPerPage - 1) / LinesPerPage;
    if (_page + 1 < pageCount) _page++;
    draw();
    return;
  }
  if (_showStart) {
    for (uint8_t i = 0; i < _recentCount; i++) {
      if (contains(_recentBounds[i], point.x, point.y)) {
        openUrl(_recentSites[i].url);
        return;
      }
    }
  }
}

void BrowserScreen::layout() {
  const int16_t width = _display.width();
  const int16_t height = _display.height();
  const int16_t buttonWidth = width / 4;
  _closeBounds = {0, 0, 42, HeaderHeight};
  _startBounds = {0, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _newBounds = {buttonWidth, (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _openBounds = {70, 68, (int16_t)(width - 140), 32};
  _previousBounds = {(int16_t)(buttonWidth * 2), (int16_t)(height - ControlsHeight), buttonWidth, ControlsHeight};
  _nextBounds = {(int16_t)(buttonWidth * 3), (int16_t)(height - ControlsHeight),
                 (int16_t)(width - buttonWidth * 3), ControlsHeight};
  for (uint8_t i = 0; i < MaxRecentSites; i++) {
    _recentBounds[i] = {8, (int16_t)(108 + i * 22), (int16_t)(width - 16), 20};
  }
}

void BrowserScreen::draw() {
  if (!_display.isReady()) return;
  layout();
  _display.tft().fillScreen(TFT_BLACK);
  drawHeader();
  if (_showStart) drawStartPage();
  else drawDocument();
  drawControls();
}

void BrowserScreen::drawHeader() {
  TFT_eSPI &tft = _display.tft();
  tft.fillRect(0, 0, _display.width(), HeaderHeight, TFT_DARKGREY);
  tft.drawLine(15, 12, 28, 19, TFT_WHITE);
  tft.drawLine(15, 26, 28, 19, TFT_WHITE);
  tft.setTextDatum(ML_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREY);
  String heading = _showStart ? "Browser" : (_title.length() ? _title : _currentUrl);
  tft.drawString(shortText(heading, _display.width() - 52), 48, HeaderHeight / 2, 2);
  tft.setTextDatum(TL_DATUM);
}

void BrowserScreen::drawStartPage() {
  TFT_eSPI &tft = _display.tft();
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Start page", _display.width() / 2, 56, 4);
  tft.fillRoundRect(70, 68, _display.width() - 140, 32, 5, TFT_BLUE);
  tft.setTextColor(TFT_WHITE, TFT_BLUE);
  tft.drawString("Open a new page", _display.width() / 2, 84, 2);
  tft.setTextDatum(TL_DATUM);

  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Recent sites", 10, 108, 2);
  if (_recentCount == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("No recent sites yet", 10, 138, 2);
    return;
  }
  for (uint8_t i = 0; i < _recentCount; i++) {
    const RecentSite &site = _recentSites[i];
    const Rect &bounds = _recentBounds[i];
    tft.drawRoundRect(bounds.x, bounds.y, bounds.w, bounds.h, 3, 0x4a69);
    String label = site.title.length() ? site.title : site.url;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(shortText(label, bounds.w - 10), bounds.x + 5, bounds.y + 2, 2);
  }
}

void BrowserScreen::drawDocument() {
  TFT_eSPI &tft = _display.tft();
  const int16_t textTop = HeaderHeight + 7;
  if (_message.length() > 0) {
    tft.setTextColor(_message == "Loading..." ? TFT_CYAN : TFT_RED, TFT_BLACK);
    tft.setTextDatum(TC_DATUM);
    tft.drawString(shortText(_message, _display.width() - 18), _display.width() / 2, textTop + 20, 2);
    tft.setTextDatum(TL_DATUM);
    return;
  }
  if (_lineCount == 0) {
    tft.setTextColor(TFT_DARKGREY, TFT_BLACK);
    tft.drawString("Page has no readable text", 10, textTop, 2);
    return;
  }
  uint16_t start = _page * LinesPerPage;
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  for (uint8_t i = 0; i < LinesPerPage && start + i < _lineCount; i++) {
    tft.drawString(_lines[start + i], 8, textTop + i * PageLineHeight, 2);
  }
}

void BrowserScreen::drawControls() {
  TFT_eSPI &tft = _display.tft();
  const Rect buttons[] = {_startBounds, _newBounds, _previousBounds, _nextBounds};
  const char *labels[] = {"Start", "New URL", "Prev", "Next"};
  for (uint8_t i = 0; i < 4; i++) {
    bool active = i < 2 || (!_showStart && ((i == 2 && _page > 0) ||
      (i == 3 && _page + 1 < (_lineCount + LinesPerPage - 1) / LinesPerPage)));
    uint16_t color = active ? 0x2965 : TFT_DARKGREY;
    const Rect &button = buttons[i];
    tft.fillRect(button.x, button.y, button.w - 1, button.h, color);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(active ? TFT_WHITE : 0x8410, color);
    tft.drawString(labels[i], button.x + button.w / 2, button.y + button.h / 2, 2);
  }
  tft.setTextDatum(TL_DATUM);
}

bool BrowserScreen::contains(const Rect &rect, int16_t x, int16_t y) const {
  return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

void BrowserScreen::openUrlInput() {
  _navigator.push(ScreenId::TextInput, ScreenData::textInput("Open URL", false));
}

void BrowserScreen::openUrl(const String &value) {
  String url;
  String error;
  if (!normalizeUrl(value, url, error)) {
    _showStart = false;
    _message = error;
    _lineCount = 0;
    draw();
    return;
  }
  if (!_wifi.isConnected()) {
    _showStart = false;
    _message = "Connect to Wi-Fi first";
    _lineCount = 0;
    draw();
    return;
  }

  _showStart = false;
  _message = "Loading...";
  _lineCount = 0;
  _title = "";
  _page = 0;
  draw();
  yield();

  String finalUrl;
  String body;
  String contentType;
  if (!fetch(url, finalUrl, body, contentType, error)) {
    _currentUrl = url;
    _message = error;
    draw();
    return;
  }
  _currentUrl = finalUrl;
  _message = "";
  if (contentType.indexOf("text/html") >= 0 || contentType.indexOf("application/xhtml") >= 0 ||
      contentType.length() == 0) {
    renderDocument(body);
  } else if (contentType.indexOf("text/plain") >= 0) {
    _title = "Text page";
    wrapRenderedText(body);
  } else {
    _title = "Unsupported content";
    _renderBuffer = "[Unsupported content: " + (contentType.length() ? contentType : String("unknown type")) + "]";
    wrapRenderedText(_renderBuffer);
  }
  addRecent(_currentUrl, _title);
  saveState();
  draw();
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
                          String &contentType, String &error) {
  String next = initialUrl;
  for (uint8_t redirect = 0; redirect <= MaxRedirects; redirect++) {
    Url url;
    if (!parseUrl(next, url)) {
      error = "Invalid redirect URL";
      return false;
    }
    int statusCode = 0;
    String location;
    if (!fetchOne(url, statusCode, location, body, contentType, error)) return false;
    if (statusCode >= 300 && statusCode <= 399) {
      if (location.length() == 0) {
        error = "Redirect has no destination";
        return false;
      }
      next = resolveRedirect(url, location);
      continue;
    }
    if (statusCode == 401) {
      error = "Basic authentication is unsupported";
      return false;
    }
    if (statusCode < 200 || statusCode >= 300) {
      error = "HTTP error " + String(statusCode);
      return false;
    }
    finalUrl = next;
    return true;
  }
  error = "Too many redirects";
  return false;
}

bool BrowserScreen::fetchOne(const Url &url, int &statusCode, String &location, String &body,
                             String &contentType, String &error) {
  _plainClient.stop();
  _secureClient.stop();
  WiFiClient *client = &_plainClient;
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
  statusCode = firstSpace >= 0 ? line.substring(firstSpace + 1).toInt() : 0;
  if (statusCode == 0) {
    client->stop();
    error = "Invalid HTTP status";
    return false;
  }

  bool chunked = false;
  int32_t contentLength = -1;
  contentType = "";
  location = "";
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
    if (name == "content-length") contentLength = headerValue.toInt();
    else if (name == "transfer-encoding" && lower(headerValue).indexOf("chunked") >= 0) chunked = true;
    else if (name == "content-type") contentType = lower(headerValue);
    else if (name == "location") location = headerValue;
    else if (name == "set-cookie") acceptCookie(headerValue, url);
  }
  bool readContent = statusCode < 300 || (statusCode >= 300 && statusCode < 400);
  if (readContent && !readBody(*client, chunked, contentLength, body, error)) {
    client->stop();
    return false;
  }
  client->stop();
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

String BrowserScreen::resolveRedirect(const Url &base, const String &location) const {
  String target = trimCopy(location);
  if (target.indexOf("://") >= 0) return target;
  String root = base.scheme + "://" + base.host;
  bool defaultPort = (base.scheme == "http" && base.port == 80) || (base.scheme == "https" && base.port == 443);
  if (!defaultPort) root += ":" + String(base.port);
  if (target.startsWith("/")) return root + target;
  String directory = base.path;
  int slash = directory.lastIndexOf('/');
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
  _renderBuffer = "";
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
    int endTag = html.indexOf('>', nextTag + 1);
    if (endTag < 0) break;
    String inside = trimCopy(html.substring(nextTag + 1, endTag));
    bool closing = inside.startsWith("/");
    if (closing) inside.remove(0, 1);
    inside.trim();
    int space = inside.indexOf(' ');
    String tag = lower(space < 0 ? inside : inside.substring(0, space));
    if (tag == "script") {
      if (!closing) sawScript = true;
      inScript = !closing;
    }
    else if (tag == "style") inStyle = !closing;
    else if (tag == "title") inTitle = !closing;
    else if (!inScript && !inStyle && !inTitle) {
      if (tag == "br" || isBlockTag(tag)) appendBreak();
      if (!closing && (tag == "img" || tag == "audio" || tag == "video" || tag == "iframe" ||
          tag == "object" || tag == "embed" || tag == "canvas")) {
        appendStub(tag == "img" ? "image" : tag);
      }
      if (!closing && (tag == "form" || tag == "input" || tag == "button" || tag == "textarea" || tag == "select")) appendStub("form control");
    }
    position = endTag + 1;
  }
  _title = decodeEntities(trimCopy(_titleBuffer));
  if (_title.length() == 0) _title = "Web page";
  _renderBuffer = decodeEntities(_renderBuffer);
  if (trimCopy(_renderBuffer).length() == 0) {
    _renderBuffer = sawScript
      ? "(This page is generated by JavaScript. JavaScript is not supported.)"
      : "(This page has no static readable text.)";
  }
  wrapRenderedText(_renderBuffer);
}

void BrowserScreen::appendText(const String &text) {
  for (uint16_t i = 0; i < text.length() && _renderBuffer.length() < 12288; i++) {
    char value = text[i];
    if (value == '\r' || value == '\n' || value == '\t') value = ' ';
    _renderBuffer += value;
  }
}

void BrowserScreen::appendBreak() {
  if (_renderBuffer.length() && !_renderBuffer.endsWith("\n")) _renderBuffer += '\n';
}

void BrowserScreen::appendStub(const String &label) {
  appendBreak();
  _renderBuffer += "(" + label + ")";
  appendBreak();
}

void BrowserScreen::wrapRenderedText(const String &text) {
  _lineCount = 0;
  String line;
  String word;
  const int16_t maxWidth = _display.width() - 16;
  auto commitLine = [&]() {
    if (_lineCount < MaxLines) _lines[_lineCount++] = line;
    line = "";
  };
  for (uint32_t i = 0; i <= text.length(); i++) {
    char value = i < text.length() ? text[i] : '\n';
    if (value != ' ' && value != '\n') {
      word += value;
      continue;
    }
    if (word.length()) {
      String candidate = line.length() ? line + " " + word : word;
      if (line.length() && _display.tft().textWidth(candidate, 2) > maxWidth) commitLine();
      while (_display.tft().textWidth(word, 2) > maxWidth && word.length() > 1) {
        String part;
        while (word.length() && _display.tft().textWidth(part + word[0], 2) <= maxWidth) {
          part += word[0];
          word.remove(0, 1);
        }
        if (_lineCount < MaxLines) _lines[_lineCount++] = part;
      }
      line = line.length() ? line + " " + word : word;
      word = "";
    }
    if (value == '\n' && line.length()) commitLine();
    if (_lineCount >= MaxLines) break;
  }
  if (_lineCount == 0) {
    _lines[_lineCount++] = "(No readable text in this response.)";
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

String BrowserScreen::lower(const String &value) const {
  String result = value;
  result.toLowerCase();
  return result;
}

bool BrowserScreen::isBlockTag(const String &tag) const {
  return tag == "p" || tag == "div" || tag == "section" || tag == "article" || tag == "header" ||
         tag == "footer" || tag == "main" || tag == "nav" || tag == "h1" || tag == "h2" || tag == "h3" ||
         tag == "h4" || tag == "h5" || tag == "h6" || tag == "ul" || tag == "ol" || tag == "li" ||
         tag == "table" || tag == "tr" || tag == "td" || tag == "th" || tag == "blockquote" || tag == "hr";
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
