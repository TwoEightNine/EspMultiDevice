// Host harness for examples/apps/dvach. It runs main.lua under the firmware's
// callback limits (100,000 instructions, 512 KiB Lua heap) against a mock
// `esp` table, answers network requests from tools/mock_dvach.py fixtures
// through the real JsonPick filter, and drives one scripted session:
// opening a board (with a mirror fallback), paging, loading thumbnails,
// reading a thread, downloading and opening files, and replying through the
// emoji captcha. Bitmap and download responses follow the firmware's
// contract and budget (32 bitmaps, 512 KiB of pixels). Build and run with
// run.sh.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#endif

#include "../../src/vendor_shims/lua/LuaConfig.h"

extern "C" {
#include "../../third_party/lua/lua.h"
#include "../../third_party/lua/lauxlib.h"
#include "../../third_party/lua/lualib.h"
}

#include "../../src/features/apps/AppLuaJson.h"
#include "../../src/features/apps/AppText.h"
#include "../../src/features/apps/JsonPick.h"

namespace {
constexpr size_t LuaMemoryLimit = 512 * 1024;
constexpr int CallbackMaxInstructions = 100000;

int failures = 0;
void check(bool condition, const std::string &what) {
  if (!condition) { ++failures; printf("FAIL: %s\n", what.c_str()); }
  else printf("ok: %s\n", what.c_str());
}

std::string fixtureDir;
std::string readFixture(const std::string &name) {
  std::ifstream in(fixtureDir + "/" + name, std::ios::binary);
  std::stringstream buffer;
  buffer << in.rdbuf();
  if (buffer.str().empty()) { printf("missing fixture %s\n", name.c_str()); exit(2); }
  return buffer.str();
}

struct BitmapDraw {
  lua_Integer handle;
  int x;
  int y;
};

struct Frame {
  std::string title;
  std::vector<std::string> texts;
  std::vector<BitmapDraw> draws;
  int commands = 0;
  int bitmaps = 0;
};

struct Request {
  int id = 0;
  std::string method;
  std::string url;
  std::string body;
  std::map<std::string, std::string> headers;
  bool json = false;
  std::vector<std::string> keep;
  uint32_t maxString = 0;
  uint32_t maxItems = 0;
  bool bitmap = false;
  int maxWidth = 320;
  int maxHeight = 432;
  std::string download;
  bool delivered = false;
  bool cancelled = false;
};

struct Bitmap {
  int width;
  int height;
  std::string path;  // empty for esp.bitmap.decode
};

lua_State *L = nullptr;
size_t luaBytes = 0;
size_t luaPeak = 0;
int hookCount = 0;
int worstInstructions = 0;
long long worstCallbackUs = 0;
// Host work the firmware excludes from the callback time limit.
long long excludedUs = 0;
std::string worstCallback;
// The firmware pays the Lua GC debt before each timed callback
// (AppRuntime::payGcDebt); DVACH_MOCK_NO_GC_PREPAY=1 turns that off to compare.
const bool prepayGc = !getenv("DVACH_MOCK_NO_GC_PREPAY") || !*getenv("DVACH_MOCK_NO_GC_PREPAY");
uint64_t clockMs = 1000;
Frame building;
Frame shown;
int presents = 0;
std::map<std::string, std::string> storage;
std::vector<Request> requests;
int nextRequestId = 1;
std::string promptTitle;
std::map<lua_Integer, Bitmap> bitmaps;
size_t maxBitmaps = 0;
lua_Integer nextBitmap = 1;
std::map<std::string, int> downloads;  // name -> bytes
std::string opened;
int appRef = LUA_NOREF;
std::string lastError;

void *luaAlloc(void *, void *pointer, size_t oldSize, size_t newSize) {
  const size_t counted = pointer ? oldSize : 0;
  if (newSize == 0) { free(pointer); luaBytes -= counted; return nullptr; }
  if (newSize > LuaMemoryLimit - (luaBytes - counted)) return nullptr;
  void *next = realloc(pointer, newSize);
  if (next) { luaBytes = luaBytes - counted + newSize; if (luaBytes > luaPeak) luaPeak = luaBytes; }
  return next;
}

void hook(lua_State *state, lua_Debug *) {
  hookCount += 1000;
  if (hookCount > CallbackMaxInstructions) luaL_error(state, "app callback exceeded its instruction limit");
}

std::string checkText(lua_State *state, int index, size_t max, const char *what) {
  size_t length = 0;
  const char *text = luaL_checklstring(state, index, &length);
  if (!length || length > max) luaL_error(state, "invalid %s (%d bytes)", what, static_cast<int>(length));
  if (!AppText::validUtf8(text, length)) luaL_error(state, "%s is not UTF-8", what);
  return std::string(text, length);
}

void countCommand(lua_State *state) {
  if (++building.commands > 64) luaL_error(state, "too many draw commands");
}

// The firmware's 400,000-pixel frame drawing budget (AppRuntime::luaDraw).
uint32_t frameCost = 0;
void charge(lua_State *state, int64_t cost) {
  if (cost < 0 || cost > 400000 - static_cast<int64_t>(frameCost))
    luaL_error(state, "frame drawing budget exceeded");
  frameCost += static_cast<uint32_t>(cost);
}

int tftBeginFrame(lua_State *state) {
  luaL_checkinteger(state, 1);
  building = Frame();
  frameCost = 0;
  building.title = checkText(state, 2, 40, "frame title");
  return 0;
}

int tftPresent(lua_State *) {
  shown = building;
  ++presents;
  return 0;
}

int tftDraw(lua_State *state) {
  countCommand(state);
  const int count = lua_gettop(state);
  const bool text = lua_toboolean(state, lua_upvalueindex(1));
  const std::string name = lua_tostring(state, lua_upvalueindex(2));
  for (int i = 1; i <= count; ++i) {
    if (text && i == 1) building.texts.push_back(checkText(state, 1, 96, "draw text"));
    else luaL_checkinteger(state, i);
  }
  auto arg = [&](int i) { return static_cast<int64_t>(lua_tointeger(state, i)); };
  int64_t cost = 1024;
  if (name == "fillScreen") cost = 320 * 432;
  else if (name == "fillRect" || name == "drawRect" || name == "fillSmoothRoundRect") cost = arg(3) * arg(4);
  else if (name == "fillSmoothCircle") cost = arg(3) * arg(3) * 4;
  else if (name == "fillTriangle") {
    const int64_t width = std::max({arg(1), arg(3), arg(5)}) - std::min({arg(1), arg(3), arg(5)}) + 1;
    const int64_t height = std::max({arg(2), arg(4), arg(6)}) - std::min({arg(2), arg(4), arg(6)}) + 1;
    cost = std::min<int64_t>(width * height, 320 * 432);
  }
  charge(state, cost);
  return 0;
}

int uiButton(lua_State *state) {
  countCommand(state);
  for (int i : {1, 2, 3, 4, 6, 7, 8}) luaL_checkinteger(state, i);
  building.texts.push_back(checkText(state, 5, 96, "button label"));
  charge(state, lua_tointeger(state, 3) * lua_tointeger(state, 4));
  return 0;
}

int uiProgress(lua_State *state) {
  countCommand(state);
  for (int i = 1; i <= 8; ++i) luaL_checkinteger(state, i);
  const lua_Integer percent = lua_tointeger(state, 5);
  if (percent < 0 || percent > 100) luaL_error(state, "invalid progress value");
  charge(state, lua_tointeger(state, 3) * lua_tointeger(state, 4));
  return 0;
}

int uiListCell(lua_State *state) {
  countCommand(state);
  luaL_checkinteger(state, 1);
  building.texts.push_back(checkText(state, 2, 96, "list cell label"));
  charge(state, 320 * 40);
  return 0;
}

int uiTextBox(lua_State *state) {
  countCommand(state);
  for (int i : {1, 2, 3, 4, 6, 7}) luaL_checkinteger(state, i);
  const lua_Integer lines = lua_tointeger(state, 4);
  if (lines < 1 || lines > 8) luaL_error(state, "invalid text box lines");
  building.texts.push_back(checkText(state, 5, 96, "text box"));
  const lua_Integer size = luaL_optinteger(state, 8, 1);
  charge(state, lua_tointeger(state, 3) * lines * (size == 0 ? 16 : size == 2 ? 27 : 21));
  return 0;
}

int uiBitmap(lua_State *state) {
  countCommand(state);
  const lua_Integer handle = luaL_checkinteger(state, 1);
  luaL_checkinteger(state, 2);
  luaL_checkinteger(state, 3);
  if (!bitmaps.count(handle)) luaL_error(state, "unknown bitmap");
  charge(state, static_cast<int64_t>(bitmaps[handle].width) * bitmaps[handle].height);
  ++building.bitmaps;
  building.draws.push_back({handle, static_cast<int>(lua_tointeger(state, 2)),
                            static_cast<int>(lua_tointeger(state, 3))});
  return 0;
}

int uiPrompt(lua_State *state) {
  promptTitle = checkText(state, 1, 40, "prompt title");
  size_t length = 0;
  luaL_optlstring(state, 2, "", &length);
  if (length > 1024) luaL_error(state, "invalid prompt text");
  lua_pushboolean(state, true);
  return 1;
}

// Approximates the firmware's font wrap with 9 px per character.
int uiWrap(lua_State *state) {
  size_t length = 0;
  const char *text = luaL_checklstring(state, 1, &length);
  const lua_Integer width = luaL_checkinteger(state, 2);
  const lua_Integer maxLines = luaL_optinteger(state, 4, 64);
  const size_t perLine = static_cast<size_t>(width / 9);
  lua_newtable(state);
  int count = 0;
  std::string line;
  size_t characters = 0;
  auto flush = [&]() {
    if (count < maxLines) {
      lua_pushlstring(state, line.data(), line.size());
      lua_rawseti(state, -2, ++count);
    }
    line.clear();
    characters = 0;
  };
  for (size_t i = 0; i < length;) {
    if (text[i] == '\n') { flush(); ++i; continue; }
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    const size_t sequence = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (characters >= perLine || line.size() + sequence > 96) flush();
    line.append(text + i, sequence);
    ++characters;
    i += sequence;
  }
  if (!line.empty()) flush();
  lua_pushboolean(state, false);
  return 2;
}

int storageRead(lua_State *state) {
  const std::string name = luaL_checkstring(state, 1);
  auto found = storage.find(name);
  if (found == storage.end()) { lua_pushnil(state); lua_pushliteral(state, "File not found"); return 2; }
  lua_pushlstring(state, found->second.data(), found->second.size());
  return 1;
}

int storageWrite(lua_State *state) {
  size_t length = 0;
  const std::string name = luaL_checkstring(state, 1);
  const char *data = luaL_checklstring(state, 2, &length);
  if (length > 16384) luaL_error(state, "app data too large");
  storage[name] = std::string(data, length);
  lua_pushboolean(state, true);
  return 1;
}

int deviceStatus(lua_State *state) {
  lua_newtable(state);
  lua_pushinteger(state, 1793003600); lua_setfield(state, -2, "epoch_seconds");
  lua_pushboolean(state, true); lua_setfield(state, -2, "wifi_connected");
  return 1;
}

int timeMillis(lua_State *state) {
  lua_pushinteger(state, static_cast<lua_Integer>(clockMs));
  return 1;
}

int netRequest(lua_State *state) {
  const int fixed = static_cast<int>(lua_tointeger(state, lua_upvalueindex(1)));
  Request request;
  int options = 0;
  static const char *const names[] = {"GET", "POST", "PUT"};
  if (fixed < 0) {
    luaL_checktype(state, 1, LUA_TTABLE);
    options = 1;
    lua_getfield(state, 1, "url"); request.url = luaL_checkstring(state, -1); lua_pop(state, 1);
    lua_getfield(state, 1, "method"); request.method = luaL_optstring(state, -1, "GET"); lua_pop(state, 1);
    lua_getfield(state, 1, "body");
    if (!lua_isnil(state, -1)) request.body = luaL_checkstring(state, -1);
    lua_pop(state, 1);
  } else {
    request.method = names[fixed];
    request.url = luaL_checkstring(state, 1);
    options = fixed == 0 ? 2 : 3;
    if (fixed) request.body = luaL_optstring(state, 2, "");
    if (lua_isnoneornil(state, options)) options = 0;
  }
  if (request.url.rfind("https://", 0) != 0 && request.url.rfind("http://", 0) != 0)
    { lua_pushnil(state); lua_pushliteral(state, "Invalid URL"); return 2; }
  if (options) {
    lua_getfield(state, options, "headers");
    if (lua_istable(state, -1)) {
      lua_pushnil(state);
      while (lua_next(state, -2)) {
        if (lua_type(state, -2) != LUA_TSTRING || lua_type(state, -1) != LUA_TSTRING)
          luaL_error(state, "header names and values must be strings");
        request.headers[lua_tostring(state, -2)] = lua_tostring(state, -1);
        lua_pop(state, 1);
      }
    }
    lua_pop(state, 1);
    lua_getfield(state, options, "json");
    if (lua_istable(state, -1)) {
      request.json = true;
      lua_getfield(state, -1, "keep");
      if (lua_istable(state, -1)) {
        const lua_Integer count = static_cast<lua_Integer>(lua_rawlen(state, -1));
        if (count < 1 || count > 32) luaL_error(state, "json.keep needs 1-32 keys");
        for (lua_Integer i = 1; i <= count; ++i) {
          lua_rawgeti(state, -1, i);
          request.keep.push_back(luaL_checkstring(state, -1));
          lua_pop(state, 1);
        }
      }
      lua_pop(state, 1);
      lua_getfield(state, -1, "max_string"); request.maxString = static_cast<uint32_t>(lua_tointeger(state, -1)); lua_pop(state, 1);
      lua_getfield(state, -1, "max_items"); request.maxItems = static_cast<uint32_t>(lua_tointeger(state, -1)); lua_pop(state, 1);
      if (request.maxString > 65536 || request.maxItems > 10000) luaL_error(state, "invalid json options");
    }
    lua_pop(state, 1);
    lua_getfield(state, options, "bitmap");
    if (lua_istable(state, -1)) {
      if (request.json) luaL_error(state, "json and bitmap together");
      request.bitmap = true;
      lua_getfield(state, -1, "max_width"); request.maxWidth = static_cast<int>(luaL_optinteger(state, -1, 320)); lua_pop(state, 1);
      lua_getfield(state, -1, "max_height"); request.maxHeight = static_cast<int>(luaL_optinteger(state, -1, 432)); lua_pop(state, 1);
      if (request.maxWidth < 1 || request.maxWidth > 320 || request.maxHeight < 1 || request.maxHeight > 432)
        luaL_error(state, "invalid bitmap options");
    }
    lua_pop(state, 1);
    lua_getfield(state, options, "download");
    if (!lua_isnil(state, -1)) {
      request.download = luaL_checkstring(state, -1);
      if (request.json || request.bitmap) luaL_error(state, "one response mode only");
      if (request.download.empty() || request.download.size() > 96 ||
          request.download.find('/') != std::string::npos || request.download[0] == '.') {
        lua_pushnil(state); lua_pushliteral(state, "Invalid download name"); return 2;
      }
    }
    lua_pop(state, 1);
    lua_getfield(state, options, "multipart");
    if (lua_istable(state, -1)) {
      const lua_Integer parts = static_cast<lua_Integer>(lua_rawlen(state, -1));
      for (lua_Integer i = 1; i <= parts; ++i) {
        lua_rawgeti(state, -1, i);
        lua_getfield(state, -1, "name");
        lua_getfield(state, -2, "value");
        request.body += std::string("name=\"") + luaL_checkstring(state, -2) + "\"\r\n\r\n" +
                        luaL_checkstring(state, -1) + "\r\n--";
        lua_pop(state, 3);
      }
      request.headers["Content-Type"] = "multipart/form-data";
    }
    lua_pop(state, 1);
  }
  request.id = nextRequestId++;
  requests.push_back(request);
  lua_pushinteger(state, request.id);
  return 1;
}

Request *findRequest(int id) {
  for (Request &request : requests) if (request.id == id && !request.delivered) return &request;
  return nullptr;
}

int netStatus(lua_State *state) {
  if (!findRequest(static_cast<int>(luaL_checkinteger(state, 1)))) { lua_pushnil(state); return 1; }
  lua_newtable(state);
  lua_pushliteral(state, "receiving"); lua_setfield(state, -2, "state");
  lua_pushinteger(state, 12345); lua_setfield(state, -2, "received");
  lua_pushinteger(state, -1); lua_setfield(state, -2, "total");
  return 1;
}

int netCancel(lua_State *state) {
  Request *request = findRequest(static_cast<int>(luaL_optinteger(state, 1, 0)));
  if (request) request->delivered = request->cancelled = true;
  return 0;
}

int downloadsStat(lua_State *state) {
  const std::string name = luaL_checkstring(state, 1);
  auto found = downloads.find(name);
  if (found == downloads.end()) { lua_pushnil(state); lua_pushliteral(state, "File not found"); return 2; }
  lua_newtable(state);
  lua_pushstring(state, name.c_str()); lua_setfield(state, -2, "name");
  lua_pushinteger(state, found->second); lua_setfield(state, -2, "size");
  return 1;
}

int downloadsOpen(lua_State *state) {
  const std::string name = luaL_checkstring(state, 1);
  if (!downloads.count(name)) { lua_pushboolean(state, false); lua_pushliteral(state, "File not found"); return 2; }
  opened = name;
  lua_pushboolean(state, true);
  return 1;
}

// Registers a bitmap under the firmware's handle and pixel limits.
const char *addBitmap(int width, int height, const std::string &path, lua_Integer &handle) {
  size_t bytes = 0;
  for (const auto &entry : bitmaps) bytes += static_cast<size_t>(entry.second.width) * entry.second.height * 2;
  if (bitmaps.size() >= 32) return "Too many bitmaps";
  if (bytes + static_cast<size_t>(width) * height * 2 > 512 * 1024) return "Bitmap memory full";
  handle = nextBitmap++;
  bitmaps[handle] = {width, height, path};
  if (bitmaps.size() > maxBitmaps) maxBitmaps = bitmaps.size();
  return nullptr;
}

size_t countBitmaps(bool thumbnails) {
  size_t count = 0;
  for (const auto &entry : bitmaps) if (entry.second.path.empty() != thumbnails) ++count;
  return count;
}

int jsonDecode(lua_State *state) {
  size_t length = 0;
  const char *text = luaL_checklstring(state, 1, &length);
  JsonPick pick(malloc, free);
  JsonPick::Options options;
  options.maxNodes = static_cast<uint32_t>(length / 2 + 2);
  options.maxStringBytes = static_cast<uint32_t>(length + 64);
  if (!pick.begin(options) || !pick.feed(reinterpret_cast<const uint8_t *>(text), length) || !pick.finish()) {
    lua_pushnil(state);
    lua_pushstring(state, pick.error());
    return 2;
  }
  lua_pushcfunction(state, AppLuaJson::pushTree);
  lua_pushlightuserdata(state, &pick);
  if (lua_pcall(state, 1, 1, 0) != LUA_OK) return lua_error(state);
  return 1;
}

int jsonEncode(lua_State *state) {
  std::string out;
  const char *error = nullptr;
  if (!AppLuaJson::encode(state, 1, out, error)) { lua_pushnil(state); lua_pushstring(state, error); return 2; }
  lua_pushlstring(state, out.data(), out.size());
  return 1;
}

int urlEncode(lua_State *state) {
  size_t length = 0;
  const char *text = luaL_checklstring(state, 1, &length);
  const std::string out = AppText::urlEncode(text, length);
  lua_pushlstring(state, out.data(), out.size());
  return 1;
}

int base64Decode(lua_State *state) {
  size_t length = 0;
  const char *text = luaL_checklstring(state, 1, &length);
  if (length > 96 * 1024) luaL_error(state, "base64 input too long");
  static const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  uint32_t accumulator = 0;
  int bits = 0;
  for (size_t i = 0; i < length && text[i] != '='; ++i) {
    const size_t value = alphabet.find(text[i]);
    if (value == std::string::npos) { lua_pushnil(state); lua_pushliteral(state, "Invalid Base64"); return 2; }
    accumulator = (accumulator << 6) | static_cast<uint32_t>(value);
    bits += 6;
    if (bits >= 8) { bits -= 8; out += static_cast<char>((accumulator >> bits) & 0xFF); }
  }
  lua_pushlstring(state, out.data(), out.size());
  return 1;
}

int hashFind(lua_State *state) {
  const std::string pattern = luaL_checkstring(state, 1);
  const std::string target = luaL_checkstring(state, 2);
  const lua_Integer limit = luaL_checkinteger(state, 3);
  const size_t marker = pattern.find("%d");
  if (marker == std::string::npos || target.size() != 128 || limit < 1 || limit > 200000)
    luaL_error(state, "invalid hash search");
#ifdef __APPLE__
  const auto searchStarted = std::chrono::steady_clock::now();
  struct Exclude {
    std::chrono::steady_clock::time_point started;
    ~Exclude() {
      excludedUs += std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    }
  } exclude{searchStarted};
  for (lua_Integer i = 0; i < limit; ++i) {
    const std::string candidate = pattern.substr(0, marker) + std::to_string(i) + pattern.substr(marker + 2);
    unsigned char digest[CC_SHA512_DIGEST_LENGTH];
    CC_SHA512(candidate.data(), static_cast<CC_LONG>(candidate.size()), digest);
    char hex[129];
    for (int k = 0; k < 64; ++k) snprintf(hex + k * 2, 3, "%02x", digest[k]);
    if (target == hex) { lua_pushinteger(state, i); return 1; }
  }
#endif
  lua_pushnil(state);
  return 1;
}

int textFromHtml(lua_State *state) {
  size_t length = 0;
  const char *html = luaL_checklstring(state, 1, &length);
  const lua_Integer max = luaL_optinteger(state, 2, 4096);
  if (length > 64 * 1024 || max < 1 || max > 16384) luaL_error(state, "invalid HTML text request");
  bool truncated = false;
  const std::string text = AppText::htmlToText(html, length, static_cast<size_t>(max), truncated);
  lua_pushlstring(state, text.data(), text.size());
  lua_pushboolean(state, truncated);
  return 2;
}

int bitmapDecode(lua_State *state) {
  size_t length = 0;
  const char *bytes = luaL_checklstring(state, 1, &length);
  if (length < 24 || memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) != 0) {
    lua_pushnil(state); lua_pushliteral(state, "Unsupported image"); return 2;
  }
  auto be32 = [&](size_t at) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 3]));
  };
  const int width = static_cast<int>(be32(16)), height = static_cast<int>(be32(20));
  lua_Integer handle = 0;
  if (const char *error = addBitmap(width, height, "", handle)) {
    lua_pushnil(state); lua_pushstring(state, error); return 2;
  }
  lua_pushinteger(state, handle);
  lua_pushinteger(state, width);
  lua_pushinteger(state, height);
  return 3;
}

int bitmapFree(lua_State *state) {
  bitmaps.erase(luaL_checkinteger(state, 1));
  return 0;
}

void setFunction(const char *name, lua_CFunction function) {
  lua_pushcfunction(L, function);
  lua_setfield(L, -2, name);
}

void registerEsp() {
  lua_newtable(L);
  lua_newtable(L);
  lua_pushcfunction(L, tftBeginFrame); lua_setfield(L, -2, "begin_frame");
  lua_pushcfunction(L, tftPresent); lua_setfield(L, -2, "present");
  for (const char *name : {"fillScreen", "fillRect", "drawRect", "drawLine", "drawFastHLine",
                           "drawFastVLine", "fillSmoothRoundRect", "fillSmoothCircle", "fillTriangle"}) {
    lua_pushboolean(L, false); lua_pushstring(L, name); lua_pushcclosure(L, tftDraw, 2);
    lua_setfield(L, -2, name);
  }
  for (const char *name : {"drawString", "drawNumber"}) {
    lua_pushboolean(L, true); lua_pushstring(L, name); lua_pushcclosure(L, tftDraw, 2);
    lua_setfield(L, -2, name);
  }
  lua_setfield(L, -2, "tft");
  lua_newtable(L);
  setFunction("button", uiButton); setFunction("progress", uiProgress);
  setFunction("list_cell", uiListCell); setFunction("text_box", uiTextBox);
  setFunction("bitmap", uiBitmap); setFunction("prompt", uiPrompt); setFunction("wrap", uiWrap);
  lua_setfield(L, -2, "ui");
  lua_newtable(L); setFunction("read", storageRead); setFunction("write", storageWrite);
  lua_setfield(L, -2, "storage");
  lua_newtable(L); setFunction("status", deviceStatus); lua_setfield(L, -2, "device");
  lua_newtable(L); setFunction("millis", timeMillis); lua_setfield(L, -2, "time");
  lua_newtable(L);
  struct Binding { const char *name; int method; };
  for (const Binding &binding : {Binding{"request", -1}, Binding{"get", 0}, Binding{"post", 1}, Binding{"put", 2}}) {
    lua_pushinteger(L, binding.method); lua_pushcclosure(L, netRequest, 1); lua_setfield(L, -2, binding.name);
  }
  setFunction("status", netStatus); setFunction("cancel", netCancel);
  lua_setfield(L, -2, "net");
  lua_newtable(L); setFunction("decode", jsonDecode); setFunction("encode", jsonEncode);
  lua_pushlightuserdata(L, AppLuaJson::nullValue()); lua_setfield(L, -2, "null");
  lua_setfield(L, -2, "json");
  lua_newtable(L); setFunction("encode", urlEncode); lua_setfield(L, -2, "url");
  lua_newtable(L); setFunction("decode", base64Decode); lua_setfield(L, -2, "base64");
  lua_newtable(L); setFunction("find_sha512", hashFind); lua_setfield(L, -2, "hash");
  lua_newtable(L); setFunction("from_html", textFromHtml); lua_setfield(L, -2, "text");
  lua_newtable(L); setFunction("decode", bitmapDecode); setFunction("free", bitmapFree);
  lua_setfield(L, -2, "bitmap");
  lua_newtable(L); setFunction("stat", downloadsStat); setFunction("open", downloadsOpen);
  lua_setfield(L, -2, "downloads");
  lua_setglobal(L, "esp");
}

// Calls app[callback] with the values pushed by `push` under the limits.
template <typename Push>
bool call(const char *callback, Push push) {
  lua_rawgeti(L, LUA_REGISTRYINDEX, appRef);
  lua_getfield(L, -1, callback);
  if (lua_isnil(L, -1)) { lua_settop(L, 0); return true; }
  const int arguments = push();
  if (prepayGc) lua_gc(L, LUA_GCSTEP, 1);
  hookCount = 0;
  lua_sethook(L, hook, LUA_MASKCOUNT, 1000);
  excludedUs = 0;
  const auto started = std::chrono::steady_clock::now();
  const int result = lua_pcall(L, arguments, 0, 0);
  const long long elapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
    std::chrono::steady_clock::now() - started).count() - excludedUs;
  if (elapsedUs > worstCallbackUs) { worstCallbackUs = elapsedUs; worstCallback = callback; }
  if (getenv("DVACH_MOCK_TRACE") && elapsedUs > 150) printf("  slow %s: %lld us\n", callback, elapsedUs);
  lua_sethook(L, nullptr, 0, 0);
  if (hookCount > worstInstructions) worstInstructions = hookCount;
  if (result != LUA_OK) {
    lastError = lua_tostring(L, -1);
    printf("Lua error in %s: %s\n", callback, lastError.c_str());
    ++failures;
    lua_settop(L, 0);
    return false;
  }
  lua_settop(L, 0);
  return true;
}

bool callNoArgs(const char *callback) { return call(callback, [] { return 0; }); }

void tick() {
  clockMs += 100;
  call("on_tick", [] { lua_pushinteger(L, static_cast<lua_Integer>(clockMs)); return 1; });
}

void tap(int x, int y, int heldMs = 60) {
  call("on_touch", [&] { lua_pushinteger(L, x); lua_pushinteger(L, y); return 2; });
  call("on_touch_end", [&] { lua_pushinteger(L, x); lua_pushinteger(L, y); lua_pushinteger(L, heldMs); return 3; });
  tick();
}

void input(const char *text) {
  call("on_input", [&] { if (text) lua_pushstring(L, text); else lua_pushnil(L); return 1; });
  callNoArgs("on_resume");
  tick();
}

std::string pathOf(const std::string &url) {
  const size_t scheme = url.find("://");
  const size_t slash = url.find('/', scheme + 3);
  return slash == std::string::npos ? "/" : url.substr(slash);
}

std::string hostOf(const std::string &url) {
  const size_t scheme = url.find("://");
  const size_t slash = url.find('/', scheme + 3);
  return url.substr(scheme + 3, slash - scheme - 3);
}

// The oldest page request; thumbnail requests are served separately.
Request *pendingRequest() {
  for (Request &request : requests) if (!request.delivered && !request.bitmap) return &request;
  return nullptr;
}

Request *pendingThumbnail() {
  for (Request &request : requests) if (!request.delivered && request.bitmap) return &request;
  return nullptr;
}

int cancelledThumbnails() {
  int count = 0;
  for (const Request &request : requests) if (request.bitmap && request.cancelled) ++count;
  return count;
}

// Builds the on_response table the way AppRuntime::luaBuildResponse does.
void respond(Request &request, int status, const std::string &contentType, const std::string &body,
             const std::map<std::string, std::string> &headers = {}) {
  request.delivered = true;
  JsonPick pick(malloc, free);
  bool parsed = false;
  std::string error;
  const bool jsonMode = request.json && status >= 200 && status < 300 &&
                        contentType.find("json") != std::string::npos;
  if (jsonMode) {
    JsonPick::Options options;
    for (size_t i = 0; i < request.keep.size(); ++i) options.keep[options.keepCount++] = request.keep[i].c_str();
    options.maxString = request.maxString;
    options.maxItems = request.maxItems;
    parsed = pick.begin(options) && pick.feed(reinterpret_cast<const uint8_t *>(body.data()), body.size()) &&
             pick.finish();
    if (!parsed) error = pick.error();
  }
  call("on_response", [&] {
    lua_pushinteger(L, request.id);
    lua_newtable(L);
    lua_pushboolean(L, error.empty() && status >= 200 && status < 300); lua_setfield(L, -2, "ok");
    lua_pushinteger(L, status); lua_setfield(L, -2, "status");
    lua_pushstring(L, request.url.c_str()); lua_setfield(L, -2, "url");
    if (!error.empty()) { lua_pushstring(L, error.c_str()); lua_setfield(L, -2, "error"); }
    lua_newtable(L);
    lua_pushstring(L, contentType.c_str()); lua_setfield(L, -2, "content-type");
    for (const auto &header : headers) { lua_pushstring(L, header.second.c_str()); lua_setfield(L, -2, header.first.c_str()); }
    lua_setfield(L, -2, "headers");
    if (jsonMode && parsed) {
      AppLuaJson::push(L, pick, 0);
      lua_setfield(L, -2, "json");
    } else if (!jsonMode) {
      const std::string kept = body.substr(0, request.json ? 4096 : 32768);
      lua_pushlstring(L, kept.data(), kept.size());
      lua_setfield(L, -2, "body");
    }
    return 2;
  });
  tick();
}

void respondFixture(Request &request, const char *name) {
  respond(request, 200, "application/json; charset=utf-8", readFixture(name));
}

// Size of a baseline JPEG from its SOF0 segment.
bool jpegSize(const std::string &data, int &width, int &height) {
  for (size_t i = 2; i + 9 < data.size();) {
    if (static_cast<uint8_t>(data[i]) != 0xFF) return false;
    const uint8_t marker = static_cast<uint8_t>(data[i + 1]);
    const size_t length = (static_cast<uint8_t>(data[i + 2]) << 8) | static_cast<uint8_t>(data[i + 3]);
    if (marker == 0xC0) {
      height = (static_cast<uint8_t>(data[i + 5]) << 8) | static_cast<uint8_t>(data[i + 6]);
      width = (static_cast<uint8_t>(data[i + 7]) << 8) | static_cast<uint8_t>(data[i + 8]);
      return true;
    }
    i += 2 + length;
  }
  return false;
}

// Answers a `bitmap` request the way AppRuntime::invokeResponse does: the
// image fitted into the box, never enlarged, then registered under the budget.
void respondBitmap(Request &request) {
  request.delivered = true;
  int width = 0, height = 0;
  const bool decoded = jpegSize(readFixture("thumb.jpg"), width, height);
  int fitWidth = width, fitHeight = height;
  if (width > request.maxWidth || height > request.maxHeight) {
    if (width * request.maxHeight >= height * request.maxWidth) {
      fitWidth = request.maxWidth;
      fitHeight = std::max(1, (height * request.maxWidth + width / 2) / width);
    } else {
      fitHeight = request.maxHeight;
      fitWidth = std::max(1, (width * request.maxHeight + height / 2) / height);
    }
  }
  lua_Integer handle = 0;
  const char *error = decoded ? addBitmap(fitWidth, fitHeight, pathOf(request.url), handle) : "Unsupported image";
  call("on_response", [&] {
    lua_pushinteger(L, request.id);
    lua_newtable(L);
    lua_pushboolean(L, error == nullptr); lua_setfield(L, -2, "ok");
    lua_pushinteger(L, 200); lua_setfield(L, -2, "status");
    lua_pushstring(L, request.url.c_str()); lua_setfield(L, -2, "url");
    lua_newtable(L); lua_setfield(L, -2, "headers");
    if (error) {
      lua_pushstring(L, error); lua_setfield(L, -2, "error");
    } else {
      lua_pushinteger(L, handle); lua_setfield(L, -2, "bitmap");
      lua_pushinteger(L, fitWidth); lua_setfield(L, -2, "width");
      lua_pushinteger(L, fitHeight); lua_setfield(L, -2, "height");
    }
    return 2;
  });
  tick();
}

int serveThumbnails() {
  int served = 0;
  while (Request *request = pendingThumbnail()) { respondBitmap(*request); ++served; }
  return served;
}

// Answers a `download` request: saved under its name, or refused with `error`.
void respondDownload(Request &request, const std::string &error = "", int bytes = 112 * 1024) {
  request.delivered = true;
  if (error.empty()) downloads[request.download] = bytes;
  call("on_response", [&] {
    lua_pushinteger(L, request.id);
    lua_newtable(L);
    lua_pushboolean(L, error.empty()); lua_setfield(L, -2, "ok");
    lua_pushinteger(L, 200); lua_setfield(L, -2, "status");
    lua_pushstring(L, request.url.c_str()); lua_setfield(L, -2, "url");
    lua_newtable(L); lua_setfield(L, -2, "headers");
    if (error.empty()) {
      lua_pushstring(L, request.download.c_str()); lua_setfield(L, -2, "downloaded");
      lua_pushinteger(L, bytes); lua_setfield(L, -2, "bytes");
    } else {
      lua_pushstring(L, error.c_str()); lua_setfield(L, -2, "error");
    }
    return 2;
  });
  tick();
}

bool shows(const std::string &needle) {
  for (const std::string &text : shown.texts) if (text.find(needle) != std::string::npos) return true;
  return false;
}

void dumpFrame() {
  printf("  frame \"%s\" (%d commands, %d bitmaps):\n", shown.title.c_str(), shown.commands, shown.bitmaps);
  for (const std::string &text : shown.texts) printf("    | %s\n", text.c_str());
  for (const BitmapDraw &draw : shown.draws) {
    auto found = bitmaps.find(draw.handle);
    printf("    [bitmap %s at %d,%d]\n", found == bitmaps.end() ? "(freed)" : found->second.path.c_str(),
           draw.x, draw.y);
  }
}

const Bitmap *drawnBitmap(const BitmapDraw &draw, const std::string &part) {
  auto found = bitmaps.find(draw.handle);
  if (found == bitmaps.end() || found->second.path.find(part) == std::string::npos) return nullptr;
  return &found->second;
}

// Taps the middle of the thumbnail drawn for a path containing `part`.
bool tapThumbnail(const std::string &part) {
  for (const BitmapDraw &draw : shown.draws) {
    const Bitmap *bitmap = drawnBitmap(draw, part);
    if (!bitmap) continue;
    tap(draw.x + bitmap->width / 2, draw.y + bitmap->height / 2);
    return true;
  }
  return false;
}

// Pages the thread forward, serving thumbnails, until `part` is drawn.
bool pageToThumbnail(const std::string &part) {
  for (int pages = 0; pages < 40; ++pages) {
    serveThumbnails();
    for (const BitmapDraw &draw : shown.draws)
      if (drawnBitmap(draw, part)) return true;
    tap(280, 400);
  }
  return false;
}
}  // namespace

int main(int argc, char **argv) {
  if (argc < 3) { printf("usage: dvach_mock main.lua fixture-dir\n"); return 2; }
  fixtureDir = argv[2];
  L = lua_newstate(luaAlloc, nullptr);
  luaL_requiref(L, "_G", luaopen_base, 1); lua_pop(L, 1);
  luaL_requiref(L, "table", luaopen_table, 1); lua_pop(L, 1);
  luaL_requiref(L, "string", luaopen_string, 1); lua_pop(L, 1);
  luaL_requiref(L, "math", luaopen_math, 1); lua_pop(L, 1);
  luaL_requiref(L, "utf8", luaopen_utf8, 1); lua_pop(L, 1);
  for (const char *name : {"dofile", "loadfile", "load", "collectgarbage"}) { lua_pushnil(L); lua_setglobal(L, name); }
  registerEsp();
  std::ifstream source(argv[1], std::ios::binary);
  std::stringstream text;
  text << source.rdbuf();
  const std::string code = text.str();
  check(code.size() <= 65536, "main.lua fits the 64 KiB source limit");
  if (luaL_loadbufferx(L, code.data(), code.size(), "dvach", "t") != LUA_OK || lua_pcall(L, 0, 1, 0) != LUA_OK) {
    printf("load error: %s\n", lua_tostring(L, -1));
    return 1;
  }
  appRef = luaL_ref(L, LUA_REGISTRYINDEX);

  callNoArgs("on_start");
  check(shown.title == "Двач" && shows("Open board..."), "home page shows the open-board button");

  // Open a board; the first mirror answers with a browser check.
  tap(160, 30);
  check(promptTitle == "Board name", "open board asks for a name");
  input("/PR/");
  Request *request = pendingRequest();
  check(request && request->url == "https://2ch.su/pr/catalog.json" && request->json,
        "catalog requested from the first mirror in JSON mode");
  tick();
  check(shows("Loading /pr/"), "loading state is shown");
  respond(*request, 403, "text/html", "<html>Just a moment</html>", {{"cf-mitigated", "challenge"}});
  request = pendingRequest();
  check(request && hostOf(request->url) == "2ch.hk", "a browser check moves to the next mirror");
  respondFixture(*request, "catalog.json");
  dumpFrame();
  lua_gc(L, LUA_GCCOLLECT);
  printf("  Lua heap live after the catalog: %zu bytes\n", luaBytes);
  check(luaBytes < 200 * 1024, "the loaded board keeps well under the 512 KiB Lua heap");
  check(shown.title == "/pr/ Программирование", "board title includes the board name");
  check(shows("Тред про программирование №0") && shows("1/10"), "threads and page count are shown");
  check(storage["settings.json"].find("\"mirror\":\"https://2ch.hk\"") != std::string::npos &&
        storage["settings.json"].find("\"id\":\"pr\"") != std::string::npos,
        "working mirror and recent board are saved");

  // Thumbnails: one request at a time for the rows on screen.
  Request *thumb = pendingThumbnail();
  check(thumb && thumb->bitmap && thumb->maxWidth == 100 && thumb->maxHeight == 100 &&
        pathOf(thumb->url) == "/pr/thumb/100/1000001s.jpg",
        "the first row's thumbnail is requested as a 100x100 bitmap");
  int inFlight = 0;
  for (const Request &r : requests) if (r.bitmap && !r.delivered) ++inFlight;
  check(inFlight == 1, "thumbnails are requested one at a time");
  check(serveThumbnails() == 3, "the three rows on the page load their thumbnails");
  dumpFrame();
  check(shown.bitmaps == 3, "the board page draws three thumbnails");
  check(shown.draws.size() == 3 && shown.draws[0].x == 4 + (100 - 86) / 2 && shown.draws[0].y == 4,
        "a 216x250 thumbnail is fitted to 86x100 and centered in its cell");
  check(shows("Тред про программирование №1") && shows("Тред про программирование №2") &&
        !shows("Тред про программирование №3"), "three threads fit on a page");

  tap(280, 400);
  check(shows("2/10"), "next page");
  check(shows("Обсуждаем тему 3"), "an empty subject falls back to the opening post text");
  check(shown.texts.size() > 0 && shown.bitmaps == 0, "page 2 waits for its thumbnails");
  tap(280, 400);
  check(shows("3/10") && cancelledThumbnails() >= 1, "paging quickly cancels a thumbnail that left the screen");
  tap(100, 400);
  tap(100, 400);
  check(shows("1/10"), "previous page");
  serveThumbnails();
  check(shown.bitmaps == 3, "returning to a page reuses or reloads its thumbnails");

  // Read the pinned thread (number 100).
  tap(100, 20);
  request = pendingRequest();
  check(request && pathOf(request->url) == "/api/mobile/v2/after/pr/100/100", "thread posts requested from the OP");
  respondFixture(*request, "after_first.json");
  serveThumbnails();
  dumpFrame();
  check(shows("#1  100") && shows("Пост номер 100."), "thread shows the OP header and text");
  check(shown.bitmaps == 3 && shown.draws[0].x == 6 + 7 && shown.draws[1].x == 110 + 7 &&
        shown.draws[2].x == 214 + 7, "the OP's three files form a strip of thumbnails");

  // Open the OP's JPEG: downloaded once into the app folder, then reopened.
  check(tapThumbnail("/1000001s.jpg"), "tap the first thumbnail");
  request = pendingRequest();
  check(request && request->download == "1000001.jpg" && pathOf(request->url) == "/pr/src/100/1000001.jpg",
        "a full file is requested as a download under its 2ch name");
  check(shown.title == "Downloading" && shows("1000001.jpg"), "a progress page shows while downloading");
  respondDownload(*request);
  check(opened == "1000001.jpg" && shown.title != "Downloading", "the saved file opens in the viewer");
  callNoArgs("on_resume");
  tick();
  check(shows("#1  100"), "back from the viewer returns to the same thread position");
  opened.clear();
  const size_t before = requests.size();
  serveThumbnails();
  check(tapThumbnail("/1000001s.jpg") && opened == "1000001.jpg" && !pendingRequest() &&
        requests.size() == before, "a file already downloaded opens without a request");
  callNoArgs("on_resume");
  opened.clear();
  check(tapThumbnail("/1000003s.jpg"), "tap the MP4 thumbnail");
  request = pendingRequest();
  check(request && request->download == "1000003.mp4", "an MP4 downloads the same way");
  respondDownload(*request);
  check(opened == "1000003.mp4", "the MP4 opens");
  opened.clear();
  callNoArgs("on_resume");
  tick();

  check(pageToThumbnail("/1010001s.jpg") && tapThumbnail("/1010001s.jpg"), "tap the WebM thumbnail");
  check(!pendingRequest() && shows("WebM can't be played on this device"), "WebM is reported, not downloaded");
  check(pageToThumbnail("/1020009s.jpg") && tapThumbnail("/1020009s.jpg"), "tap the 30 MB file");
  check(shown.title == "Download" && shows("30.0 MB. Download it?") && !pendingRequest(),
        "a large file asks before downloading");
  tap(200, 400);
  request = pendingRequest();
  check(request && request->download == "1020009.mp4" && shown.title == "Downloading", "Download starts it");
  Request *big = request;
  tap(60, 400);
  check(big->cancelled && !pendingRequest() && shown.title != "Downloading" && !downloads.count("1020009.mp4"),
        "Cancel stops the download and returns to the thread");
  check(pageToThumbnail("/1030066s.jpg") && tapThumbnail("/1030066s.jpg"), "tap the mislabeled file");
  request = pendingRequest();
  respondDownload(*request, "Unsupported file download");
  check(shows("Download failed: Unsupported file download") && opened.empty(),
        "a file whose content does not match its name is refused");
  check(pageToThumbnail("/1040004s.jpg"), "a post with four files gets a second strip");
  check(pageToThumbnail("/1050003s.jpg"), "a strip of three videos draws within the frame budget");
  check(countBitmaps(true) <= 16, "the app keeps at most 16 thumbnails");
  lua_gc(L, LUA_GCCOLLECT);
  printf("  Lua heap live with the board and a thread batch: %zu bytes\n", luaBytes);
  check(luaBytes < 300 * 1024, "board plus thread stay well under the 512 KiB Lua heap");
  int pages = 0;
  while (!shows("More") && pages++ < 200) tap(280, 400);
  check(shows("More"), "paging reaches the end of the first batch");
  tap(280, 400);
  request = pendingRequest();
  check(request && pathOf(request->url) == "/api/mobile/v2/after/pr/100/125", "More requests the next posts");
  respondFixture(*request, "after_rest.json");
  check(shows("#26  125"), "the next batch is shown");

  // Reply through the emoji captcha.
  tap(180, 400);
  check(promptTitle == "Reply to #100", "reply opens the keyboard");
  input("Привет, мир");
  request = pendingRequest();
  check(request && pathOf(request->url) == "/api/captcha/emoji/id?board=pr&thread=100",
        "captcha id requested for the board and thread");
  respondFixture(*request, "captcha_id.json");
  request = pendingRequest();
  check(request && pathOf(request->url).rfind("/api/captcha/emoji/show?id=", 0) == 0, "captcha image requested");
  respondFixture(*request, "captcha_show.json");
  dumpFrame();
  check(shown.title == "Captcha" && shown.bitmaps == 9 && !pendingThumbnail(),
        "captcha image and eight keys are drawn, and no thumbnails are requested");
  tap(40, 190);
  request = pendingRequest();
  check(request && request->method == "POST" && request->body == "{\"captchaTokenID\":\"mock-captcha-token\",\"emojiNumber\":0}",
        "an emoji tap posts the click as JSON");
  respondFixture(*request, "captcha_next.json");
  check(shown.bitmaps == 9 && countBitmaps(false) == 9, "the next captcha step replaces the bitmaps");
  tap(120, 260);
  request = pendingRequest();
  respondFixture(*request, "captcha_success.json");
  request = pendingRequest();
  check(request && pathOf(request->url) == "/user/posting?nc=1", "the post is sent after the captcha");
  check(request && request->body.find("name=\"emoji_captcha_id\"\r\n\r\nmock-success-token") != std::string::npos &&
        request->body.find("name=\"2ch_challenge\"\r\n\r\n4242") != std::string::npos &&
        request->body.find("name=\"comment\"\r\n\r\nПривет, мир") != std::string::npos &&
        request->body.find("name=\"captcha_type\"\r\n\r\nemoji_captcha") != std::string::npos,
        "posting fields carry the captcha token, solved challenge, and comment");
  check(countBitmaps(false) == 0, "captcha bitmaps are freed after solving");
  respondFixture(*request, "posting_ok.json");
  check(shows("Posted #135"), "success is reported");

  // Unknown board and removing a recent board.
  tap(30, 400);
  tap(30, 400);
  check(shown.title == "Двач" && shows("/pr/  Программирование"), "back to home with the recent board");
  check(bitmaps.empty(), "leaving the board frees every thumbnail");
  tap(160, 30);
  input("zzqx");
  request = pendingRequest();
  respond(*request, 404, "text/html; charset=utf-8", "<html>Not found</html>");
  check(shown.title == "Двач" && shows("No board /zzqx/"), "an unknown board is reported");
  tap(100, 150, 1200);
  check(!shows("/pr/  Программирование") && shows("Removed /pr/"), "holding a recent board removes it");

  // Thumbnails off: compact rows and no image requests.
  tap(160, 80);
  check(shows("Show thumbnails") && storage["settings.json"].find("\"thumbs\":false") != std::string::npos,
        "the Home toggle turns thumbnails off and saves it");

  // A failed load offers Retry on the board and in a thread.
  tap(160, 30);
  input("pr");
  request = pendingRequest();
  respond(*request, 500, "text/html", "<html>Server error</html>");
  check(shows("Retry") && shows("Couldn't load: HTTP 500"), "a failed board load offers Retry");
  tap(180, 400);
  request = pendingRequest();
  check(request && pathOf(request->url) == "/pr/catalog.json", "Retry reloads the board");
  respondFixture(*request, "catalog.json");
  check(!shows("Retry") && shows("1/5") && !pendingThumbnail() && shown.bitmaps == 0,
        "with thumbnails off the board has six rows a page and requests no images");
  tap(100, 20);
  request = pendingRequest();
  respond(*request, 0, "", "", {});
  check(shows("Retry"), "a failed thread load offers Retry");
  tap(280, 400);
  request = pendingRequest();
  check(request && pathOf(request->url) == "/api/mobile/v2/after/pr/100/100", "Retry reloads the thread");
  respondFixture(*request, "after_first.json");
  check(shows("#1  100") && !pendingThumbnail() && shows("JPG") && shows("MP4"),
        "with thumbnails off a thread shows file placeholders without requests");

  printf("\nLua heap peak %zu bytes, worst callback %d instructions, %d frames\n",
         luaPeak, worstInstructions, presents);
  printf("slowest callback on this host: %s %lld us (GC prepaid: %s)\n",
         worstCallback.c_str(), worstCallbackUs, prepayGc ? "yes" : "no");
  // On the device the Lua heap is in PSRAM and roughly 1,000 instructions
  // take a millisecond, so keep each callback far below the 20 ms limit.
  check(worstInstructions <= 10000, "every callback stays well under the device time limit");
  printf("most bitmaps held at once: %zu\n", maxBitmaps);
  check(maxBitmaps <= 32, "the bitmap handle budget is never exceeded");
  lua_close(L);
  if (failures) { printf("%d check(s) failed\n", failures); return 1; }
  printf("dvach_mock: all checks passed\n");
  return 0;
}
