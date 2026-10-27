// SDK 6 additions to AppRuntime: network requests, JSON, URL, Base64, hashes,
// HTML-to-text, wrapped text, decoded bitmaps, and the app downloads folder.
#include "AppRuntime.h"

#include <FS.h>
#include <SD.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <math.h>
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha512.h>

#include <algorithm>
#include <new>
#include <string>
#include <vector>

#include "../../vendor_shims/lua/LuaConfig.h"

extern "C" {
#include "../../../third_party/lua/lua.h"
#include "../../../third_party/lua/lauxlib.h"
#include "../../../third_party/lua/lualib.h"
}

#include "../../core/SpiBusLock.h"
#include "../../media/FileFormats.h"
#include "../../services/storage/PathPolicy.h"
#include "../../services/storage/SdCardController.h"
#include "AppBitmapDecode.h"
#include "AppLuaJson.h"
#include "AppNetWorker.h"
#include "AppPackage.h"
#include "AppText.h"
#include "AppTextLayout.h"
#include "JsonPick.h"

namespace {
constexpr size_t MaxDecodeBytes = 32 * 1024;
constexpr size_t MaxEncodeBytes = 16 * 1024;
constexpr size_t MaxBase64Input = 96 * 1024;
constexpr size_t MaxHashInput = 96 * 1024;
constexpr size_t MaxHtmlInput = 64 * 1024;
constexpr size_t MaxWrapInput = 16 * 1024;
constexpr uint32_t MaxHashSearch = 200000;

int pushFailure(lua_State *state, bool nilFirst, const char *error) {
  if (nilFirst) lua_pushnil(state); else lua_pushboolean(state, false);
  lua_pushstring(state, error);
  return 2;
}

void copyText(char *target, size_t capacity, const char *source) {
  snprintf(target, capacity, "%s", source ? source : "");
}

void setFunction(lua_State *state, AppRuntime *runtime, lua_CFunction function, const char *name) {
  lua_pushlightuserdata(state, runtime);
  lua_pushcclosure(state, function, 1);
  lua_setfield(state, -2, name);
}

std::string hex(const uint8_t *bytes, size_t length) {
  static const char digits[] = "0123456789abcdef";
  std::string out(length * 2, '0');
  for (size_t i = 0; i < length; ++i) {
    out[i * 2] = digits[bytes[i] >> 4];
    out[i * 2 + 1] = digits[bytes[i] & 15];
  }
  return out;
}

// Reads an optional integer field in [low, high]; false with an error pushed.
bool optionalInteger(lua_State *state, int table, const char *name, lua_Integer low,
                     lua_Integer high, lua_Integer &value) {
  lua_getfield(state, table, name);
  if (lua_isnil(state, -1)) { lua_pop(state, 1); return true; }
  int isInteger = 0;
  const lua_Integer number = lua_tointegerx(state, -1, &isInteger);
  lua_pop(state, 1);
  if (!isInteger || number < low || number > high) {
    luaL_error(state, "invalid %s", name);
    return false;
  }
  value = number;
  return true;
}

// Fills JSON filter options from a Lua table at `table`.
void readJsonOptions(lua_State *state, int table, JsonPick::Options &options,
                     char keep[][JsonPick::MaxKeyBytes + 1]) {
  lua_getfield(state, table, "keep");
  if (!lua_isnil(state, -1)) {
    luaL_checktype(state, -1, LUA_TTABLE);
    const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, -1));
    if (count < 1 || count > JsonPick::MaxKeepKeys) luaL_error(state, "json.keep needs 1-32 keys");
    for (lua_Integer i = 1; i <= count; ++i) {
      lua_rawgeti(state, -1, i);
      size_t length = 0;
      const char *key = lua_type(state, -1) == LUA_TSTRING ? lua_tolstring(state, -1, &length) : nullptr;
      if (!key || !length || length > JsonPick::MaxKeyBytes) luaL_error(state, "invalid json.keep key");
      memcpy(keep[i - 1], key, length);
      keep[i - 1][length] = 0;
      options.keep[i - 1] = keep[i - 1];
      lua_pop(state, 1);
    }
    options.keepCount = static_cast<uint8_t>(count);
  }
  lua_pop(state, 1);
  lua_Integer value = 0;
  if (optionalInteger(state, table, "max_string", 0, 65536, value)) options.maxString = static_cast<uint32_t>(value);
  value = 0;
  if (optionalInteger(state, table, "max_items", 0, 10000, value)) options.maxItems = static_cast<uint32_t>(value);
}

// A request being assembled. Lua errors while reading the options unwind past
// C++ destructors, so the pending request lives in a userdata box whose __gc
// frees whatever was not handed to the network worker.
struct PendingRequest {
  AppNetWorker::Spec *spec = nullptr;
  std::string multipart;
  std::string boundary;
};

void freePending(PendingRequest *&pending) {
  if (!pending) return;
  if (pending->spec) {
    AppNetWorker::free(pending->spec->body);
    delete pending->spec;
  }
  delete pending;
  pending = nullptr;
}

int collectPending(lua_State *state) {
  PendingRequest **box = static_cast<PendingRequest **>(lua_touserdata(state, 1));
  if (box) freePending(*box);
  return 0;
}

// Collects a string-keyed table of scalars as sorted `key=value` pairs.
bool collectPairs(lua_State *state, int index, std::vector<std::pair<std::string, std::string>> &pairs,
                  const char *&error) {
  index = lua_absindex(state, index);
  lua_pushnil(state);
  while (lua_next(state, index)) {
    if (lua_type(state, -2) != LUA_TSTRING) { lua_pop(state, 2); error = "Query keys must be strings"; return false; }
    size_t keyLength = 0;
    const char *key = lua_tolstring(state, -2, &keyLength);
    std::string value;
    switch (lua_type(state, -1)) {
      case LUA_TSTRING: case LUA_TNUMBER: {
        size_t length = 0;
        lua_pushvalue(state, -1);
        const char *text = lua_tolstring(state, -1, &length);
        value.assign(text, length);
        lua_pop(state, 1);
        break;
      }
      case LUA_TBOOLEAN: value = lua_toboolean(state, -1) ? "true" : "false"; break;
      default: lua_pop(state, 2); error = "Query values must be strings, numbers, or booleans"; return false;
    }
    pairs.emplace_back(std::string(key, keyLength), value);
    lua_pop(state, 1);
  }
  std::sort(pairs.begin(), pairs.end());
  return true;
}

// Reads `{max_width, max_height, background}` from the table at `table`.
void readBitmapOptions(lua_State *state, int table, AppBitmapDecode::Options &options) {
  lua_Integer value = 0;
  if (optionalInteger(state, table, "max_width", 1, AppBitmapDecode::MaxWidth, value) && value)
    options.maxWidth = static_cast<uint16_t>(value);
  value = 0;
  if (optionalInteger(state, table, "max_height", 1, AppBitmapDecode::MaxHeight, value) && value)
    options.maxHeight = static_cast<uint16_t>(value);
  value = -1;
  if (optionalInteger(state, table, "background", 0, 65535, value) && value >= 0)
    options.background = static_cast<uint16_t>(value);
}

// A plain file name for the app downloads folder: one valid path segment of
// 1-96 bytes with an image, audio, or video extension.
bool validDownloadName(const char *name, size_t length) {
  if (!name || !length || length > AppNetWorker::MaxDownloadNameBytes ||
      memchr(name, '/', length) || memchr(name, 0, length)) return false;
  String path;
  const String candidate = String("/") + name;
  if (!PathPolicy::appPath(candidate.c_str(), candidate.length(), false, path)) return false;
  return FileFormats::isDownloadable(FileFormats::fromPath(candidate));
}

// What luaBuildResponse turns into the `response` table.
struct ResponseView {
  const AppNetWorker::Result *result;
  lua_Integer bitmap;  // 0 when the response carries none
};
}  // namespace

void AppRuntime::registerSdk6() {
  lua_getfield(_lua, -1, "ui");
  setFunction(_lua, this, luaTextBox, "text_box");
  setFunction(_lua, this, luaDrawBitmap, "bitmap");
  setFunction(_lua, this, luaUiWrap, "wrap");
  lua_pop(_lua, 1);

  if (_networkPermission) {
    lua_newtable(_lua);
    struct Binding { const char *name; int method; };
    static const Binding bindings[] = {
      {"request", -1}, {"get", static_cast<int>(HttpClient::Method::Get)},
      {"post", static_cast<int>(HttpClient::Method::Post)},
      {"put", static_cast<int>(HttpClient::Method::Put)}};
    for (const Binding &binding : bindings) {
      lua_pushlightuserdata(_lua, this);
      lua_pushinteger(_lua, binding.method);
      lua_pushcclosure(_lua, luaNetRequest, 2);
      lua_setfield(_lua, -2, binding.name);
    }
    setFunction(_lua, this, luaNetStatus, "status");
    setFunction(_lua, this, luaNetCancel, "cancel");
    lua_setfield(_lua, -2, "net");
  }

  lua_newtable(_lua);
  setFunction(_lua, this, luaJsonDecode, "decode");
  setFunction(_lua, this, luaJsonEncode, "encode");
  lua_pushlightuserdata(_lua, AppLuaJson::nullValue());
  lua_setfield(_lua, -2, "null");
  lua_setfield(_lua, -2, "json");

  lua_newtable(_lua);
  setFunction(_lua, this, luaUrlEncode, "encode");
  setFunction(_lua, this, luaUrlDecode, "decode");
  setFunction(_lua, this, luaUrlQuery, "query");
  lua_setfield(_lua, -2, "url");

  lua_newtable(_lua);
  setFunction(_lua, this, luaBase64Encode, "encode");
  setFunction(_lua, this, luaBase64Decode, "decode");
  lua_setfield(_lua, -2, "base64");

  lua_newtable(_lua);
  for (int algorithm : {256, 512}) {
    lua_pushlightuserdata(_lua, this);
    lua_pushinteger(_lua, algorithm);
    lua_pushcclosure(_lua, luaHash, 2);
    lua_setfield(_lua, -2, algorithm == 256 ? "sha256" : "sha512");
  }
  setFunction(_lua, this, luaHashFind, "find_sha512");
  lua_setfield(_lua, -2, "hash");

  lua_newtable(_lua);
  setFunction(_lua, this, luaTextFromHtml, "from_html");
  lua_setfield(_lua, -2, "text");

  lua_newtable(_lua);
  setFunction(_lua, this, luaBitmapDecode, "decode");
  setFunction(_lua, this, luaBitmapFree, "free");
  lua_setfield(_lua, -2, "bitmap");

  if (_networkPermission) {
    lua_newtable(_lua);
    setFunction(_lua, this, luaDownloadsStat, "stat");
    setFunction(_lua, this, luaDownloadsOpen, "open");
    lua_setfield(_lua, -2, "downloads");
  }
}

int AppRuntime::luaNetRequest(lua_State *state) {
  AppRuntime *runtime = self(state);
  if (!runtime->_net) return pushFailure(state, true, "Network permission required");
  const int fixedMethod = static_cast<int>(lua_tointeger(state, lua_upvalueindex(2)));
  int optionsIndex = 0;
  size_t urlLength = 0;
  const char *url = nullptr;
  const char *body = nullptr;
  size_t bodyLength = 0;
  HttpClient::Method method = HttpClient::Method::Get;
  if (fixedMethod < 0) {
    luaL_checktype(state, 1, LUA_TTABLE);
    optionsIndex = 1;
    lua_getfield(state, 1, "url");
    url = luaL_checklstring(state, -1, &urlLength);
    lua_pop(state, 1);  // The options table keeps the string alive.
    lua_getfield(state, 1, "method");
    static const char *const methods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", nullptr};
    method = static_cast<HttpClient::Method>(luaL_checkoption(state, -1, "GET", methods));
    lua_pop(state, 1);
    lua_getfield(state, 1, "body");
    if (!lua_isnil(state, -1)) body = luaL_checklstring(state, -1, &bodyLength);
    lua_pop(state, 1);
  } else {
    method = static_cast<HttpClient::Method>(fixedMethod);
    url = luaL_checklstring(state, 1, &urlLength);
    if (method == HttpClient::Method::Get) {
      optionsIndex = lua_isnoneornil(state, 2) ? 0 : 2;
    } else {
      if (!lua_isnoneornil(state, 2)) body = luaL_checklstring(state, 2, &bodyLength);
      optionsIndex = lua_isnoneornil(state, 3) ? 0 : 3;
    }
    if (optionsIndex) luaL_checktype(state, optionsIndex, LUA_TTABLE);
  }
  // Refusals are logged by reason only, like HttpClient's own lines.
  auto refuse = [&](const char *reason) {
    Serial.printf("[App net] request refused: %s\n", reason);
    return pushFailure(state, true, reason);
  };
  if (memchr(url, 0, urlLength)) return refuse("Invalid URL");
  HttpClient::Url parsed;
  if (!HttpClient::parseUrl(String(url), parsed)) return refuse("Invalid URL");
  if (!runtime->_net->hostAllowed(parsed.host)) return refuse("Host not allowed");

  PendingRequest **box = static_cast<PendingRequest **>(lua_newuserdatauv(state, sizeof(PendingRequest *), 0));
  *box = nullptr;
  if (luaL_newmetatable(state, "esp.net.pending")) {
    lua_pushcfunction(state, collectPending);
    lua_setfield(state, -2, "__gc");
  }
  lua_setmetatable(state, -2);
  *box = new (std::nothrow) PendingRequest();
  if (*box) (*box)->spec = new (std::nothrow) AppNetWorker::Spec();
  if (!*box || !(*box)->spec) {
    freePending(*box);
    return pushFailure(state, true, "App memory unavailable");
  }
  AppNetWorker::Spec *spec = (*box)->spec;
  std::string &multipart = (*box)->multipart;
  std::string &boundary = (*box)->boundary;
  spec->method = method;
  spec->url = url;
  spec->followRedirects = method == HttpClient::Method::Get;
  bool contentType = false;
  const char *failure = nullptr;
  if (optionsIndex) {
    lua_Integer value = 0;
    if (optionalInteger(state, optionsIndex, "timeout_ms", 1000, 60000, value) && value)
      spec->timeoutMs = static_cast<uint32_t>(value);
    value = 0;
    if (optionalInteger(state, optionsIndex, "max_bytes", 1, AppNetWorker::MaxTextBytes, value) && value)
      spec->maxBytes = static_cast<uint32_t>(value);
    lua_getfield(state, optionsIndex, "redirects");
    if (!lua_isnil(state, -1)) spec->followRedirects = lua_toboolean(state, -1);
    lua_pop(state, 1);
    lua_getfield(state, optionsIndex, "headers");
    if (!lua_isnil(state, -1)) {
      luaL_checktype(state, -1, LUA_TTABLE);
      lua_pushnil(state);
      while (!failure && lua_next(state, -2)) {
        if (lua_type(state, -2) != LUA_TSTRING || lua_type(state, -1) != LUA_TSTRING) {
          failure = "Invalid header";
        } else {
          size_t nameLength = 0, valueLength = 0;
          const char *name = lua_tolstring(state, -2, &nameLength);
          const char *headerValue = lua_tolstring(state, -1, &valueLength);
          String lowerName(name);
          lowerName.toLowerCase();
          if (!HttpClient::validHeaderName(name, nameLength) ||
              !HttpClient::validHeaderValue(headerValue, valueLength) ||
              HttpClient::reservedHeader(lowerName)) {
            failure = "Invalid header";
          } else if (spec->headerCount >= HttpClient::MaxRequestHeaders) {
            failure = "Too many headers";
          } else {
            if (lowerName == "content-type") contentType = true;
            spec->headers[spec->headerCount++] = {String(name), String(headerValue)};
          }
        }
        lua_pop(state, 1);
      }
      if (failure) lua_pop(state, 1);  // The pending key.
    }
    lua_pop(state, 1);
    lua_getfield(state, optionsIndex, "json");
    if (!failure && !lua_isnil(state, -1)) {
      luaL_checktype(state, -1, LUA_TTABLE);
      spec->json = true;
      spec->jsonOptions = JsonPick::Options();
      readJsonOptions(state, lua_gettop(state), spec->jsonOptions, spec->keepText);
    }
    lua_pop(state, 1);
    lua_getfield(state, optionsIndex, "bitmap");
    if (!failure && !lua_isnil(state, -1)) {
      luaL_checktype(state, -1, LUA_TTABLE);
      if (spec->json) failure = "Use one of json, bitmap, or download";
      spec->bitmap = true;
      readBitmapOptions(state, lua_gettop(state), spec->bitmapOptions);
    }
    lua_pop(state, 1);
    lua_getfield(state, optionsIndex, "download");
    if (!failure && !lua_isnil(state, -1)) {
      size_t nameLength = 0;
      const char *name = luaL_checklstring(state, -1, &nameLength);
      if (spec->json || spec->bitmap) failure = "Use one of json, bitmap, or download";
      else if (!validDownloadName(name, nameLength)) failure = "Invalid download name";
      else if (!runtime->_card.isMounted()) failure = "SD card unavailable";
      else copyText(spec->download, sizeof(spec->download), name);
    }
    lua_pop(state, 1);
    lua_getfield(state, optionsIndex, "multipart");
    if (!failure && !lua_isnil(state, -1)) {
      luaL_checktype(state, -1, LUA_TTABLE);
      if (body) failure = "Use body or multipart, not both";
      char random[17];
      snprintf(random, sizeof(random), "%08lx%08lx", static_cast<unsigned long>(esp_random()),
               static_cast<unsigned long>(esp_random()));
      boundary = std::string("----ESPMediaBoundary") + random;
      const lua_Integer parts = static_cast<lua_Integer>(lua_rawlen(state, -1));
      if (!failure && (parts < 1 || parts > 32)) failure = "Multipart needs 1-32 parts";
      for (lua_Integer i = 1; !failure && i <= parts; ++i) {
        lua_rawgeti(state, -1, i);
        luaL_checktype(state, -1, LUA_TTABLE);
        size_t nameLength = 0, valueLength = 0, fileLength = 0, typeLength = 0;
        lua_getfield(state, -1, "name");
        const char *name = luaL_checklstring(state, -1, &nameLength);
        lua_getfield(state, -2, "value");
        const char *partValue = luaL_optlstring(state, -1, "", &valueLength);
        lua_getfield(state, -3, "filename");
        const char *filename = luaL_optlstring(state, -1, nullptr, &fileLength);
        lua_getfield(state, -4, "content_type");
        const char *type = luaL_optlstring(state, -1, nullptr, &typeLength);
        auto safe = [](const char *text, size_t length) {
          for (size_t k = 0; k < length; ++k)
            if (text[k] == '"' || text[k] == '\r' || text[k] == '\n' || text[k] == 0) return false;
          return true;
        };
        if (!nameLength || nameLength > 64 || !safe(name, nameLength) ||
            (filename && (fileLength > 128 || !safe(filename, fileLength))) ||
            (type && !HttpClient::validHeaderValue(type, typeLength))) {
          failure = "Invalid multipart part";
        } else {
          multipart += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"";
          multipart.append(name, nameLength);
          multipart += "\"";
          if (filename) { multipart += "; filename=\""; multipart.append(filename, fileLength); multipart += "\""; }
          multipart += "\r\n";
          if (type) { multipart += "Content-Type: "; multipart.append(type, typeLength); multipart += "\r\n"; }
          multipart += "\r\n";
          multipart.append(partValue, valueLength);
          multipart += "\r\n";
          if (multipart.size() > AppNetWorker::MaxBodyBytes) failure = "Body too large";
        }
        lua_pop(state, 5);
      }
      if (!failure) multipart += "--" + boundary + "--\r\n";
    }
    lua_pop(state, 1);
  }
  if (!failure && multipart.size()) {
    body = multipart.data();
    bodyLength = multipart.size();
    // The boundary must match the body; an app content type is replaced.
    uint8_t kept = 0;
    for (uint8_t i = 0; i < spec->headerCount; ++i) {
      String name = spec->headers[i].name;
      name.toLowerCase();
      if (name != "content-type") spec->headers[kept++] = spec->headers[i];
    }
    spec->headerCount = kept;
    if (spec->headerCount >= HttpClient::MaxRequestHeaders) failure = "Too many headers";
    else spec->headers[spec->headerCount++] = {"Content-Type", String("multipart/form-data; boundary=") + boundary.c_str()};
    contentType = true;
  }
  if (!failure && bodyLength > AppNetWorker::MaxBodyBytes) failure = "Body too large";
  if (!failure && body && !contentType && method != HttpClient::Method::Get) {
    if (spec->headerCount >= HttpClient::MaxRequestHeaders) failure = "Too many headers";
    else spec->headers[spec->headerCount++] = {"Content-Type", "application/x-www-form-urlencoded"};
  }
  if (!failure && body && bodyLength) {
    spec->body = static_cast<uint8_t *>(AppNetWorker::allocate(bodyLength));
    if (!spec->body) failure = "App memory unavailable";
    else { memcpy(spec->body, body, bodyLength); spec->bodyLength = bodyLength; }
  }
  if (!failure && WiFi.status() != WL_CONNECTED) failure = "Wi-Fi not connected";
  if (failure) {
    freePending(*box);
    return refuse(failure);
  }
  (*box)->spec = nullptr;
  freePending(*box);
  String error;
  const uint32_t id = runtime->_net->submit(spec, error);
  if (!id) return refuse(error.c_str());
  lua_pushinteger(state, static_cast<lua_Integer>(id));
  return 1;
}

int AppRuntime::luaNetStatus(lua_State *state) {
  AppRuntime *runtime = self(state);
  const lua_Integer id = luaL_checkinteger(state, 1);
  AppNetWorker::State requestState = AppNetWorker::State::Free;
  uint32_t received = 0;
  int32_t total = -1;
  if (!runtime->_net || id <= 0 ||
      !runtime->_net->status(static_cast<uint32_t>(id), requestState, received, total) ||
      requestState == AppNetWorker::State::Done) {
    lua_pushnil(state);
    return 1;
  }
  static const char *const names[] = {"free", "queued", "connecting", "sending", "receiving", "done"};
  lua_createtable(state, 0, 3);
  lua_pushstring(state, names[static_cast<int>(requestState)]); lua_setfield(state, -2, "state");
  lua_pushinteger(state, received); lua_setfield(state, -2, "received");
  lua_pushinteger(state, total); lua_setfield(state, -2, "total");
  return 1;
}

int AppRuntime::luaNetCancel(lua_State *state) {
  AppRuntime *runtime = self(state);
  const lua_Integer id = luaL_optinteger(state, 1, 0);
  if (runtime->_net && id >= 0) runtime->_net->cancel(static_cast<uint32_t>(id));
  return 0;
}

bool AppRuntime::serviceNet() {
  if (_paused || !_net) return true;
  AppNetWorker::Result *result = nullptr;
  if (!_net->take(result)) return true;
  return invokeResponse(result);
}

int AppRuntime::luaBuildResponse(lua_State *state) {
  const ResponseView *view = static_cast<const ResponseView *>(lua_touserdata(state, 1));
  const AppNetWorker::Result *result = view ? view->result : nullptr;
  lua_createtable(state, 0, 8);
  if (!result) {
    lua_pushboolean(state, false); lua_setfield(state, -2, "ok");
    lua_pushinteger(state, 0); lua_setfield(state, -2, "status");
    lua_pushliteral(state, "App memory full"); lua_setfield(state, -2, "error");
    return 1;
  }
  const bool transport = result->error.length() == 0;
  lua_pushboolean(state, transport && result->status >= 200 && result->status < 300);
  lua_setfield(state, -2, "ok");
  lua_pushinteger(state, result->status); lua_setfield(state, -2, "status");
  lua_pushstring(state, result->finalUrl.c_str()); lua_setfield(state, -2, "url");
  lua_pushboolean(state, result->truncated); lua_setfield(state, -2, "truncated");
  if (!transport) { lua_pushstring(state, result->error.c_str()); lua_setfield(state, -2, "error"); }
  lua_createtable(state, 0, result->headerCount);
  for (uint8_t i = 0; i < result->headerCount; ++i) {
    const HttpClient::Header &header = result->headers[i];
    lua_getfield(state, -1, header.name.c_str());
    if (lua_isstring(state, -1)) {
      // Repeated headers such as set-cookie are joined by newlines.
      lua_pushliteral(state, "\n");
      lua_pushstring(state, header.value.c_str());
      lua_concat(state, 3);
    } else {
      lua_pop(state, 1);
      lua_pushstring(state, header.value.c_str());
    }
    lua_setfield(state, -2, header.name.c_str());
  }
  lua_setfield(state, -2, "headers");
  if (result->body) {
    lua_pushlstring(state, result->body, result->bodyLength);
    lua_setfield(state, -2, "body");
  }
  if (result->json && result->json->nodeCount()) {
    AppLuaJson::push(state, *result->json, 0);
    lua_setfield(state, -2, "json");
  }
  if (view->bitmap) {
    lua_pushinteger(state, view->bitmap); lua_setfield(state, -2, "bitmap");
    lua_pushinteger(state, result->width); lua_setfield(state, -2, "width");
    lua_pushinteger(state, result->height); lua_setfield(state, -2, "height");
  }
  if (result->downloaded.length()) {
    lua_pushstring(state, result->downloaded.c_str()); lua_setfield(state, -2, "downloaded");
    lua_pushinteger(state, result->downloadedBytes); lua_setfield(state, -2, "bytes");
  }
  return 1;
}

bool AppRuntime::invokeResponse(void *pointer) {
  AppNetWorker::Result *result = static_cast<AppNetWorker::Result *>(pointer);
  const lua_Integer id = static_cast<lua_Integer>(result->id);
  lua_rawgeti(_lua, LUA_REGISTRYINDEX, _appRef);
  lua_getfield(_lua, -1, "on_response");
  if (lua_isnil(_lua, -1)) {
    lua_settop(_lua, 0);
    AppNetWorker::release(result);
    return true;
  }
  if (!lua_isfunction(_lua, -1)) {
    lua_settop(_lua, 0);
    AppNetWorker::release(result);
    setError("Invalid app callback");
    return false;
  }
  lua_pushinteger(_lua, id);
  // A decoded bitmap joins the app's budget only now; a full budget turns
  // the response into an error.
  ResponseView view{result, 0};
  if (result->pixels) {
    int64_t handle = 0;
    const char *failure = nullptr;
    if (registerBitmap(result->pixels, result->width, result->height, handle, failure)) {
      view.bitmap = static_cast<lua_Integer>(handle);
    } else {
      result->error = failure;
      Serial.printf("[SD app] %s: response bitmap rejected: %s\n", _id.c_str(), failure);
    }
    result->pixels = nullptr;
  }
  // Converting a large response may exhaust the Lua heap; build it protected
  // so the app receives an error instead of stopping.
  lua_pushcfunction(_lua, luaBuildResponse);
  lua_pushlightuserdata(_lua, &view);
  int built = lua_pcall(_lua, 1, 1, 0);
  AppNetWorker::release(result);
  if (built != LUA_OK) {
    // The app never saw the handle.
    if (view.bitmap && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
      Bitmap &bitmap = _bitmaps[view.bitmap & 0x1f];
      heap_caps_free(bitmap.pixels);
      _bitmapBytes -= static_cast<size_t>(bitmap.width) * bitmap.height * 2;
      bitmap = Bitmap{};
      xSemaphoreGive(_mutex);
    }
    lua_pop(_lua, 1);
    lua_gc(_lua, LUA_GCCOLLECT);
    ResponseView failed{nullptr, 0};
    lua_pushcfunction(_lua, luaBuildResponse);
    lua_pushlightuserdata(_lua, &failed);
    built = lua_pcall(_lua, 1, 1, 0);
    if (built != LUA_OK) {
      lua_settop(_lua, 0);
      setError("App memory full");
      return false;
    }
  }
  _callbackName = "on_response";
  return callPrepared(2);
}

int AppRuntime::luaJsonDecode(lua_State *state) {
  AppRuntime *runtime = self(state);
  size_t length = 0;
  const char *text = luaL_checklstring(state, 1, &length);
  if (length > MaxDecodeBytes) return pushFailure(state, true, "JSON too large");
  JsonPick::Options options;
  char keep[JsonPick::MaxKeepKeys][JsonPick::MaxKeyBytes + 1];
  if (!lua_isnoneornil(state, 2)) {
    luaL_checktype(state, 2, LUA_TTABLE);
    readJsonOptions(state, 2, options, keep);
  }
  options.maxNodes = static_cast<uint32_t>(length / 2 + 2);
  options.maxStringBytes = static_cast<uint32_t>(length + 64);
  const int64_t started = callbackClockUs();
  JsonPick *pick = new (std::nothrow) JsonPick(AppNetWorker::allocate, AppNetWorker::free);
  const bool parsed = pick && pick->begin(options) &&
                      pick->feed(reinterpret_cast<const uint8_t *>(text), length) && pick->finish();
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!parsed) {
    lua_pushnil(state);
    lua_pushstring(state, pick && pick->error() ? pick->error() : "App memory unavailable");
    delete pick;
    return 2;
  }
  lua_pushcfunction(state, AppLuaJson::pushTree);
  lua_pushlightuserdata(state, pick);
  const int status = lua_pcall(state, 1, 1, 0);
  const bool truncated = pick->truncated();
  delete pick;
  if (status != LUA_OK) return lua_error(state);
  lua_pushboolean(state, truncated);
  return 2;
}

int AppRuntime::luaJsonEncode(lua_State *state) {
  luaL_checkany(state, 1);
  std::string out;
  const char *error = nullptr;
  bool ok = AppLuaJson::encode(state, 1, out, error);
  if (ok && out.size() > MaxEncodeBytes) { ok = false; error = "JSON too large"; }
  if (!ok) {
    lua_pushnil(state);
    lua_pushstring(state, error ? error : "Cannot encode value");
    return 2;
  }
  lua_pushlstring(state, out.data(), out.size());
  return 1;
}

int AppRuntime::luaUrlEncode(lua_State *state) {
  size_t length = 0;
  const char *text = luaL_checklstring(state, 1, &length);
  if (length > 16384) return luaL_error(state, "text too long");
  const std::string encoded = AppText::urlEncode(text, length);
  lua_pushlstring(state, encoded.data(), encoded.size());
  return 1;
}

int AppRuntime::luaUrlDecode(lua_State *state) {
  size_t length = 0;
  const char *text = luaL_checklstring(state, 1, &length);
  if (length > 16384) return luaL_error(state, "text too long");
  std::string decoded;
  if (!AppText::urlDecode(text, length, lua_toboolean(state, 2), decoded))
    return pushFailure(state, true, "Invalid percent-encoding");
  lua_pushlstring(state, decoded.data(), decoded.size());
  return 1;
}

int AppRuntime::luaUrlQuery(lua_State *state) {
  luaL_checktype(state, 1, LUA_TTABLE);
  std::vector<std::pair<std::string, std::string>> pairs;
  const char *error = nullptr;
  if (!collectPairs(state, 1, pairs, error)) return luaL_error(state, "%s", error);
  std::string query;
  for (size_t i = 0; i < pairs.size(); ++i) {
    if (i) query += '&';
    query += AppText::urlEncode(pairs[i].first.data(), pairs[i].first.size());
    query += '=';
    query += AppText::urlEncode(pairs[i].second.data(), pairs[i].second.size());
    if (query.size() > AppNetWorker::MaxBodyBytes) return luaL_error(state, "query too long");
  }
  lua_pushlstring(state, query.data(), query.size());
  return 1;
}

int AppRuntime::luaBase64Encode(lua_State *state) {
  size_t length = 0;
  const char *bytes = luaL_checklstring(state, 1, &length);
  if (length > MaxBase64Input) return luaL_error(state, "base64 input too long");
  luaL_Buffer buffer;
  const size_t capacity = 4 * ((length + 2) / 3) + 1;
  unsigned char *out = reinterpret_cast<unsigned char *>(luaL_buffinitsize(state, &buffer, capacity));
  size_t written = 0;
  if (mbedtls_base64_encode(out, capacity, &written, reinterpret_cast<const unsigned char *>(bytes), length) != 0)
    return luaL_error(state, "base64 encoding failed");
  luaL_pushresultsize(&buffer, written);
  return 1;
}

int AppRuntime::luaBase64Decode(lua_State *state) {
  AppRuntime *runtime = self(state);
  size_t length = 0;
  const char *text = luaL_checklstring(state, 1, &length);
  if (length > MaxBase64Input) return pushFailure(state, true, "Base64 input too long");
  const int64_t started = callbackClockUs();
  luaL_Buffer buffer;
  const size_t capacity = (length / 4) * 3 + 3;
  unsigned char *out = reinterpret_cast<unsigned char *>(luaL_buffinitsize(state, &buffer, capacity));
  size_t written = 0;
  const int result = mbedtls_base64_decode(out, capacity, &written,
                                           reinterpret_cast<const unsigned char *>(text), length);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (result != 0) {
    luaL_pushresultsize(&buffer, 0);
    lua_pop(state, 1);
    return pushFailure(state, true, "Invalid Base64");
  }
  luaL_pushresultsize(&buffer, written);
  return 1;
}

int AppRuntime::luaHash(lua_State *state) {
  AppRuntime *runtime = self(state);
  const int algorithm = static_cast<int>(lua_tointeger(state, lua_upvalueindex(2)));
  size_t length = 0;
  const char *bytes = luaL_checklstring(state, 1, &length);
  if (length > MaxHashInput) return luaL_error(state, "hash input too long");
  const int64_t started = callbackClockUs();
  uint8_t digest[64];
  const unsigned char *input = reinterpret_cast<const unsigned char *>(bytes);
  if (algorithm == 256) mbedtls_sha256(input, length, digest, 0);
  else mbedtls_sha512(input, length, digest, 0);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  const std::string text = hex(digest, algorithm == 256 ? 32 : 64);
  lua_pushlstring(state, text.data(), text.size());
  return 1;
}

int AppRuntime::luaHashFind(lua_State *state) {
  AppRuntime *runtime = self(state);
  size_t templateLength = 0, targetLength = 0;
  const char *pattern = luaL_checklstring(state, 1, &templateLength);
  const char *target = luaL_checklstring(state, 2, &targetLength);
  const lua_Integer limit = luaL_checkinteger(state, 3);
  const char *marker = templateLength <= 512 ? strstr(pattern, "%d") : nullptr;
  if (!marker || strstr(marker + 2, "%d") || memchr(pattern, 0, templateLength) ||
      targetLength != 128 || limit < 1 || limit > MaxHashSearch)
    return luaL_error(state, "invalid hash search");
  uint8_t wanted[64];
  for (size_t i = 0; i < 64; ++i) {
    char pair[3] = {target[i * 2], target[i * 2 + 1], 0};
    char *end = nullptr;
    wanted[i] = static_cast<uint8_t>(strtoul(pair, &end, 16));
    if (!end || *end) return luaL_error(state, "invalid hash search");
  }
  const size_t prefix = static_cast<size_t>(marker - pattern);
  const char *suffix = marker + 2;
  const size_t suffixLength = templateLength - prefix - 2;
  const int64_t started = callbackClockUs();
  lua_Integer found = -1;
  mbedtls_sha512_context context;
  uint8_t digest[64];
  for (lua_Integer candidate = 0; candidate < limit && !runtime->_cancel; ++candidate) {
    char number[24];
    const int numberLength = snprintf(number, sizeof(number), "%lld", static_cast<long long>(candidate));
    mbedtls_sha512_init(&context);
    mbedtls_sha512_starts(&context, 0);
    mbedtls_sha512_update(&context, reinterpret_cast<const unsigned char *>(pattern), prefix);
    mbedtls_sha512_update(&context, reinterpret_cast<const unsigned char *>(number), numberLength);
    mbedtls_sha512_update(&context, reinterpret_cast<const unsigned char *>(suffix), suffixLength);
    mbedtls_sha512_finish(&context, digest);
    mbedtls_sha512_free(&context);
    if (memcmp(digest, wanted, sizeof(wanted)) == 0) { found = candidate; break; }
    if ((candidate & 1023) == 1023) taskYIELD();
  }
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (found < 0) lua_pushnil(state);
  else lua_pushinteger(state, found);
  return 1;
}

int AppRuntime::luaTextFromHtml(lua_State *state) {
  AppRuntime *runtime = self(state);
  size_t length = 0;
  const char *html = luaL_checklstring(state, 1, &length);
  const lua_Integer maxBytes = luaL_optinteger(state, 2, 4096);
  if (length > MaxHtmlInput || maxBytes < 1 || maxBytes > 16384)
    return luaL_error(state, "invalid HTML text request");
  const int64_t started = callbackClockUs();
  bool truncated = false;
  const std::string text = AppText::htmlToText(html, length, static_cast<size_t>(maxBytes), truncated);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  lua_pushlstring(state, text.data(), text.size());
  lua_pushboolean(state, truncated);
  return 2;
}

int AppRuntime::luaUiWrap(lua_State *state) {
  AppRuntime *runtime = self(state);
  size_t length = 0;
  const char *text = luaL_checklstring(state, 1, &length);
  const lua_Integer width = luaL_checkinteger(state, 2);
  const lua_Integer size = luaL_optinteger(state, 3, 1);
  const lua_Integer maxLines = luaL_optinteger(state, 4, 64);
  if (length > MaxWrapInput || width < 16 || width > 640 || size < 0 || size > 2 ||
      maxLines < 1 || maxLines > 256)
    return luaL_error(state, "invalid wrap request");
  const int64_t started = callbackClockUs();
  AppTextLayout::Line lines[256];
  bool more = false;
  const uint16_t count = AppTextLayout::wrap(text, length, static_cast<int16_t>(width),
                                             static_cast<UiText::Size>(size), lines,
                                             static_cast<uint16_t>(maxLines), 96, more);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  lua_createtable(state, count, 0);
  for (uint16_t i = 0; i < count; ++i) {
    lua_pushlstring(state, text + lines[i].start, lines[i].length);
    lua_rawseti(state, -2, i + 1);
  }
  lua_pushboolean(state, more);
  return 2;
}

int AppRuntime::luaTextBox(lua_State *state) {
  AppRuntime *runtime = self(state);
  if (!runtime->_frameOpen || !runtime->_buildingFrame)
    return luaL_error(state, "begin_frame required");
  Frame &frame = *runtime->_buildingFrame;
  if (frame.count >= MaxDrawCommands) return luaL_error(state, "too many draw commands");
  const lua_Integer x = luaL_checkinteger(state, 1);
  const lua_Integer y = luaL_checkinteger(state, 2);
  const lua_Integer width = luaL_checkinteger(state, 3);
  const lua_Integer lines = luaL_checkinteger(state, 4);
  size_t length = 0;
  const char *text = luaL_checklstring(state, 5, &length);
  const lua_Integer color = luaL_checkinteger(state, 6);
  const lua_Integer background = luaL_checkinteger(state, 7);
  const lua_Integer size = luaL_optinteger(state, 8, 1);
  if (x < -512 || x > 1024 || y < -512 || y > 1024 || width < 16 || width > 640 ||
      lines < 1 || lines > 8 || color < 0 || color > 65535 || background < 0 ||
      background > 65535 || size < 0 || size > 2)
    return luaL_error(state, "draw argument out of range");
  if (!length || length > 96 || memchr(text, 0, length)) return luaL_error(state, "invalid draw text");
  const uint32_t cost = static_cast<uint32_t>(width * lines * UiText::lineHeight(static_cast<UiText::Size>(size)));
  if (cost > 400000UL - runtime->_frameCost) return luaL_error(state, "frame drawing budget exceeded");
  runtime->_frameCost += cost;
  DrawCommand &command = frame.commands[frame.count++];
  command = DrawCommand{};
  command.op = DrawOp::TextBox;
  command.arg[0] = static_cast<int32_t>(x);
  command.arg[1] = static_cast<int32_t>(y);
  command.arg[2] = static_cast<int32_t>(width);
  command.arg[3] = static_cast<int32_t>(lines);
  command.arg[4] = static_cast<int32_t>(color);
  command.arg[5] = static_cast<int32_t>(background);
  command.arg[6] = static_cast<int32_t>(size);
  copyText(command.text, sizeof(command.text), text);
  return 0;
}

int AppRuntime::luaDrawBitmap(lua_State *state) {
  AppRuntime *runtime = self(state);
  if (!runtime->_frameOpen || !runtime->_buildingFrame)
    return luaL_error(state, "begin_frame required");
  Frame &frame = *runtime->_buildingFrame;
  if (frame.count >= MaxDrawCommands) return luaL_error(state, "too many draw commands");
  const lua_Integer handle = luaL_checkinteger(state, 1);
  const lua_Integer x = luaL_checkinteger(state, 2);
  const lua_Integer y = luaL_checkinteger(state, 3);
  if (x < -512 || x > 1024 || y < -512 || y > 1024) return luaL_error(state, "draw argument out of range");
  const uint8_t slot = static_cast<uint8_t>(handle & 0x1f);
  const Bitmap &bitmap = runtime->_bitmaps[slot];
  if (handle <= 0 || !bitmap.pixels || bitmap.serial != static_cast<uint32_t>(handle >> 5))
    return luaL_error(state, "unknown bitmap");
  const uint32_t cost = static_cast<uint32_t>(bitmap.width) * bitmap.height;
  if (cost > 400000UL - runtime->_frameCost) return luaL_error(state, "frame drawing budget exceeded");
  runtime->_frameCost += cost;
  DrawCommand &command = frame.commands[frame.count++];
  command = DrawCommand{};
  command.op = DrawOp::Bitmap;
  command.arg[0] = static_cast<int32_t>(x);
  command.arg[1] = static_cast<int32_t>(y);
  command.arg[2] = static_cast<int32_t>(handle);
  return 0;
}

bool AppRuntime::registerBitmap(uint16_t *pixels, uint16_t width, uint16_t height,
                                int64_t &handle, const char *&error) {
  const size_t bytes = static_cast<size_t>(width) * height * 2;
  int slot = -1;
  error = nullptr;
  if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    error = "Host busy";
  } else {
    for (int i = 0; i < MaxBitmaps; ++i) if (!_bitmaps[i].pixels) { slot = i; break; }
    if (slot < 0) error = "Too many bitmaps";
    else if (bytes > MaxBitmapBytes - _bitmapBytes) error = "Bitmap memory full";
    else {
      _bitmapSerial = (_bitmapSerial + 1) & 0x03ffffffu;
      if (!_bitmapSerial) _bitmapSerial = 1;
      _bitmaps[slot] = {_bitmapSerial, width, height, pixels};
      _bitmapBytes += bytes;
      handle = (static_cast<int64_t>(_bitmapSerial) << 5) | slot;
    }
    xSemaphoreGive(_mutex);
  }
  if (error) heap_caps_free(pixels);
  return error == nullptr;
}

int AppRuntime::luaBitmapDecode(lua_State *state) {
  AppRuntime *runtime = self(state);
  size_t length = 0;
  const char *bytes = luaL_checklstring(state, 1, &length);
  AppBitmapDecode::Options options;
  if (lua_istable(state, 2)) {
    readBitmapOptions(state, 2, options);
  } else {
    const lua_Integer background = luaL_optinteger(state, 2, 0);
    if (background < 0 || background > 65535) return luaL_error(state, "invalid background color");
    options.background = static_cast<uint16_t>(background);
  }
  if (length > AppBitmapDecode::MaxSourceBytes) return pushFailure(state, true, "Image too large");
  const uint8_t *data = reinterpret_cast<const uint8_t *>(bytes);
  if (!AppBitmapDecode::supported(data, length)) return pushFailure(state, true, "Unsupported image");
  // Refuse before decoding when no handle could be issued.
  bool slotFree = false;
  for (const Bitmap &bitmap : runtime->_bitmaps) if (!bitmap.pixels) { slotFree = true; break; }
  if (!slotFree) return pushFailure(state, true, "Too many bitmaps");
  const int64_t started = callbackClockUs();
  const char *failure = nullptr;
  uint16_t width = 0, height = 0;
  uint16_t *pixels = AppBitmapDecode::decode(data, length, options, MaxBitmapBytes - runtime->_bitmapBytes,
                                             width, height, failure);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!pixels) return pushFailure(state, true, failure ? failure : "Cannot decode image");
  int64_t handle = 0;
  if (!runtime->registerBitmap(pixels, width, height, handle, failure))
    return pushFailure(state, true, failure);
  lua_pushinteger(state, static_cast<lua_Integer>(handle));
  lua_pushinteger(state, width);
  lua_pushinteger(state, height);
  return 3;
}

int AppRuntime::luaBitmapFree(lua_State *state) {
  AppRuntime *runtime = self(state);
  const lua_Integer handle = luaL_checkinteger(state, 1);
  if (handle <= 0) return 0;
  const uint8_t slot = static_cast<uint8_t>(handle & 0x1f);
  if (xSemaphoreTake(runtime->_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return luaL_error(state, "host busy");
  Bitmap &bitmap = runtime->_bitmaps[slot];
  if (bitmap.pixels && bitmap.serial == static_cast<uint32_t>(handle >> 5)) {
    heap_caps_free(bitmap.pixels);
    runtime->_bitmapBytes -= static_cast<size_t>(bitmap.width) * bitmap.height * 2;
    bitmap = Bitmap{};
  }
  xSemaphoreGive(runtime->_mutex);
  return 0;
}

bool AppRuntime::lockBitmap(int32_t handle, const uint16_t *&pixels, uint16_t &width, uint16_t &height) {
  if (handle <= 0 || !_mutex || xSemaphoreTake(_mutex, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  const Bitmap &bitmap = _bitmaps[handle & 0x1f];
  if (!bitmap.pixels || bitmap.serial != static_cast<uint32_t>(handle >> 5)) {
    xSemaphoreGive(_mutex);
    return false;
  }
  pixels = bitmap.pixels;
  width = bitmap.width;
  height = bitmap.height;
  return true;
}

void AppRuntime::unlockBitmap() {
  if (_mutex) xSemaphoreGive(_mutex);
}

void AppRuntime::releaseBitmaps() {
  for (Bitmap &bitmap : _bitmaps) {
    if (bitmap.pixels) heap_caps_free(bitmap.pixels);
    bitmap = Bitmap{};
  }
  _bitmapBytes = 0;
}

bool AppRuntime::checkDownloadName(lua_State *state, int index, bool nilOnError, String &path) {
  size_t length = 0;
  const char *name = luaL_checklstring(state, index, &length);
  const char *error = nullptr;
  if (!_downloads.length()) error = "Network permission required";
  else if (!validDownloadName(name, length)) error = "Invalid download name";
  else if (!_card.isMounted()) error = "SD card unavailable";
  if (!error) {
    path = _downloads + "/" + name;
    return true;
  }
  pushFailure(state, nilOnError, error);
  return false;
}

int AppRuntime::luaDownloadsStat(lua_State *state) {
  AppRuntime *runtime = self(state);
  String path;
  if (!runtime->checkDownloadName(state, 1, true, path)) return 2;
  const int64_t started = callbackClockUs();
  bool found = false;
  uint32_t size = 0, modified = 0;
  const bool locked = SpiBusLock::lock(AppPackage::SdLockTimeout);
  if (locked) {
    fs::File file = SD.open(path.c_str(), FILE_READ);
    found = file && !file.isDirectory();
    if (found) {
      size = static_cast<uint32_t>(file.size());
      modified = static_cast<uint32_t>(file.getLastWrite());
    }
    if (file) file.close();
    SpiBusLock::unlock();
  }
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!locked) return pushFailure(state, true, "SD busy");
  if (!found) return pushFailure(state, true, "File not found");
  const String name = path.substring(path.lastIndexOf('/') + 1);
  pushFsEntry(state, name.c_str(), false, size, modified,
              static_cast<uint8_t>(FileFormats::fromPath(name)));
  return 1;
}

int AppRuntime::luaDownloadsOpen(lua_State *state) {
  AppRuntime *runtime = self(state);
  String path;
  if (!runtime->checkDownloadName(state, 1, false, path)) return 2;
  if (runtime->_navigationPending) return pushFailure(state, false, "Navigation pending");
  const int64_t started = callbackClockUs();
  bool found = false;
  const bool locked = SpiBusLock::lock(AppPackage::SdLockTimeout);
  if (locked) {
    fs::File file = SD.open(path.c_str(), FILE_READ);
    found = file && !file.isDirectory();
    if (file) file.close();
    SpiBusLock::unlock();
  }
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!locked) return pushFailure(state, false, "SD busy");
  if (!found) return pushFailure(state, false, "File not found");
  return runtime->requestOpenFile(state, path);
}
