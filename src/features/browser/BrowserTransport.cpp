#include "BrowserTransport.h"

#include "../../services/network/WiFiController.h"

BrowserTransport::BrowserTransport(WiFiController &wifi) : _wifi(wifi), _mutex(xSemaphoreCreateMutex()) {}

void BrowserTransport::stopClients() {
  _plainClient.stop();
  _secureClient.stop();
}

bool BrowserTransport::normalizeUrl(const String &value, String &normalized, String &error) {
  normalized = trimCopy(value); error = "";
  if (!normalized.length()) { error = "Enter a URL"; return false; }
  if (normalized.indexOf("://") < 0) normalized = "https://" + normalized;
  Browser::Url url{};
  if (!parseUrl(normalized, url)) { error = "Only valid HTTP/HTTPS URLs work"; return false; }
  normalized = url.scheme + "://" + url.host;
  const bool defaultPort = (url.scheme == "http" && url.port == 80) || (url.scheme == "https" && url.port == 443);
  if (!defaultPort) normalized += ":" + String(url.port);
  normalized += url.path;
  return true;
}

bool BrowserTransport::parseUrl(const String &value, Browser::Url &url) {
  const int schemeEnd = value.indexOf("://"); if (schemeEnd <= 0) return false;
  url.scheme = lower(value.substring(0, schemeEnd)); if (url.scheme != "http" && url.scheme != "https") return false;
  const int authorityStart = schemeEnd + 3; int pathStart = value.indexOf('/', authorityStart); const int queryStart = value.indexOf('?', authorityStart);
  if (pathStart < 0 || (queryStart >= 0 && queryStart < pathStart)) pathStart = queryStart;
  String authority = pathStart < 0 ? value.substring(authorityStart) : value.substring(authorityStart, pathStart);
  if (!authority.length() || authority.indexOf('@') >= 0) return false;
  const int colon = authority.lastIndexOf(':'); url.port = url.scheme == "https" ? 443 : 80; url.host = authority;
  if (colon >= 0) { const String portText = authority.substring(colon + 1); const long port = portText.toInt();
    if (port <= 0 || port > 65535 || String(port) != portText) return false;
    url.port = (uint16_t)port; url.host = authority.substring(0, colon);
  }
  url.host.trim(); if (!url.host.length() || url.host.indexOf(' ') >= 0 || url.host.indexOf('[') >= 0) return false;
  url.host = lower(url.host); url.path = pathStart < 0 ? "/" : value.substring(pathStart);
  if (url.path.startsWith("?")) url.path = "/" + url.path;
  const int fragment = url.path.indexOf('#'); if (fragment >= 0) url.path.remove(fragment);
  if (!url.path.length()) url.path = "/";
  return true;
}

String BrowserTransport::resolveUrl(const Browser::Url &base, const String &reference) {
  String target = trimCopy(reference); String root = base.scheme + "://" + base.host;
  const bool defaultPort = (base.scheme == "http" && base.port == 80) || (base.scheme == "https" && base.port == 443);
  if (!defaultPort) root += ":" + String(base.port);
  int fragment = target.indexOf('#'); if (fragment >= 0) target.remove(fragment); if (!target.length()) return root + base.path;
  if (target.startsWith("//")) return base.scheme + ":" + target; if (target.indexOf("://") >= 0) return target;
  const int colon = target.indexOf(':'); int separator = target.length(); int slash = target.indexOf('/'); const int query = target.indexOf('?');
  if (slash >= 0 && slash < separator) separator = slash; if (query >= 0 && query < separator) separator = query;
  if (colon >= 0 && colon < separator) return target; if (target.startsWith("/")) return root + target;
  String path = base.path; const int queryStart = path.indexOf('?'); if (queryStart >= 0) path.remove(queryStart);
  if (target.startsWith("?")) return root + path + target; String directory = path; slash = directory.lastIndexOf('/');
  directory = slash >= 0 ? directory.substring(0, slash + 1) : "/"; return root + directory + target;
}

String BrowserTransport::trimCopy(const String &value) { String result = value; result.trim(); return result; }
String BrowserTransport::lower(const String &value) { String result = value; result.toLowerCase(); return result; }
