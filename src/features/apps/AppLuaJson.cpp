#include "AppLuaJson.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../vendor_shims/lua/LuaConfig.h"

extern "C" {
#include "../../../third_party/lua/lua.h"
#include "../../../third_party/lua/lauxlib.h"
}

#include "AppText.h"
#include "JsonPick.h"

namespace AppLuaJson {
namespace {
constexpr size_t MaxEncodeBytes = 16 * 1024;
constexpr uint8_t MaxEncodeDepth = 16;
// `esp.json.null` is this address as a light userdata.
char jsonNull;

bool encodeValue(lua_State *state, int index, uint8_t depth, std::string &out, const char *&error);

bool encodeString(const char *text, size_t length, std::string &out, const char *&error) {
  if (!AppText::validUtf8(text, length)) { error = "String is not UTF-8"; return false; }
  out += '"';
  for (size_t i = 0; i < length; ++i) {
    const uint8_t c = static_cast<uint8_t>(text[i]);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char escaped[8];
          snprintf(escaped, sizeof(escaped), "\\u%04x", c);
          out += escaped;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
  return true;
}

bool encodeValue(lua_State *state, int index, uint8_t depth, std::string &out, const char *&error) {
  if (out.size() > MaxEncodeBytes) { error = "JSON too large"; return false; }
  index = lua_absindex(state, index);
  switch (lua_type(state, index)) {
    case LUA_TNIL: out += "null"; return true;
    case LUA_TBOOLEAN: out += lua_toboolean(state, index) ? "true" : "false"; return true;
    case LUA_TLIGHTUSERDATA:
      if (lua_touserdata(state, index) == &jsonNull) { out += "null"; return true; }
      error = "Cannot encode userdata";
      return false;
    case LUA_TNUMBER: {
      char text[40];
      if (lua_isinteger(state, index)) {
        snprintf(text, sizeof(text), "%lld", static_cast<long long>(lua_tointeger(state, index)));
      } else {
        const lua_Number number = lua_tonumber(state, index);
        if (!isfinite(number)) { error = "Cannot encode NaN or infinity"; return false; }
        snprintf(text, sizeof(text), "%.17g", static_cast<double>(number));
      }
      out += text;
      return true;
    }
    case LUA_TSTRING: {
      size_t length = 0;
      const char *text = lua_tolstring(state, index, &length);
      return encodeString(text, length, out, error);
    }
    case LUA_TTABLE: {
      if (depth >= MaxEncodeDepth) { error = "JSON too deep"; return false; }
      if (!lua_checkstack(state, 4)) { error = "JSON too deep"; return false; }
      size_t count = 0;
      bool allIntegers = true;
      bool allStrings = true;
      lua_Integer largest = 0;
      lua_pushnil(state);
      while (lua_next(state, index)) {
        ++count;
        if (lua_type(state, -2) == LUA_TNUMBER && lua_isinteger(state, -2)) {
          largest = std::max(largest, lua_tointeger(state, -2));
          allStrings = false;
        } else if (lua_type(state, -2) == LUA_TSTRING) {
          allIntegers = false;
        } else {
          lua_pop(state, 2);
          error = "Table keys must be strings or array indexes";
          return false;
        }
        lua_pop(state, 1);
      }
      if (count && allIntegers && largest == static_cast<lua_Integer>(count)) {
        out += '[';
        for (size_t i = 1; i <= count; ++i) {
          if (i > 1) out += ',';
          lua_rawgeti(state, index, static_cast<lua_Integer>(i));
          const bool ok = encodeValue(state, -1, depth + 1, out, error);
          lua_pop(state, 1);
          if (!ok) return false;
        }
        out += ']';
        return true;
      }
      if (count && !allStrings) { error = "Mixed table keys"; return false; }
      std::vector<std::string> keys;
      keys.reserve(count);
      lua_pushnil(state);
      while (lua_next(state, index)) {
        size_t length = 0;
        const char *key = lua_tolstring(state, -2, &length);
        keys.emplace_back(key, length);
        lua_pop(state, 1);
      }
      std::sort(keys.begin(), keys.end());
      out += '{';
      for (size_t i = 0; i < keys.size(); ++i) {
        if (i) out += ',';
        if (!encodeString(keys[i].data(), keys[i].size(), out, error)) return false;
        out += ':';
        lua_pushlstring(state, keys[i].data(), keys[i].size());
        lua_rawget(state, index);
        const bool ok = encodeValue(state, -1, depth + 1, out, error);
        lua_pop(state, 1);
        if (!ok) return false;
      }
      out += '}';
      return true;
    }
    default:
      error = "Cannot encode this value";
      return false;
  }
}
}  // namespace

void *nullValue() { return &jsonNull; }

uint32_t push(lua_State *state, const JsonPick &pick, uint32_t index) {
  luaL_checkstack(state, 4, "JSON too deep");
  const JsonPick::Node &node = pick.nodes()[index];
  switch (node.type) {
    case JsonPick::Type::Null: lua_pushlightuserdata(state, &jsonNull); return index + 1;
    case JsonPick::Type::True: lua_pushboolean(state, true); return index + 1;
    case JsonPick::Type::False: lua_pushboolean(state, false); return index + 1;
    case JsonPick::Type::String:
      lua_pushlstring(state, pick.strings() + node.offset, node.length);
      return index + 1;
    case JsonPick::Type::Number: {
      char text[48];
      const size_t length = node.length < sizeof(text) - 1 ? node.length : sizeof(text) - 1;
      memcpy(text, pick.strings() + node.offset, length);
      text[length] = 0;
      if (!lua_stringtonumber(state, text)) lua_pushnumber(state, strtod(text, nullptr));
      return index + 1;
    }
    case JsonPick::Type::Array:
    case JsonPick::Type::Object: {
      const bool object = node.type == JsonPick::Type::Object;
      lua_createtable(state, object ? 0 : static_cast<int>(node.count), object ? static_cast<int>(node.count) : 0);
      uint32_t child = index + 1;
      for (uint32_t i = 0; i < node.count; ++i) {
        const JsonPick::Node &member = pick.nodes()[child];
        if (object) lua_pushlstring(state, pick.strings() + member.keyOffset, member.keyLength);
        child = push(state, pick, child);
        if (object) lua_rawset(state, -3);
        else lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
      }
      return node.end;
    }
  }
  lua_pushnil(state);
  return index + 1;
}

int pushTree(lua_State *state) {
  const JsonPick *pick = static_cast<const JsonPick *>(lua_touserdata(state, 1));
  if (!pick->nodeCount()) lua_pushnil(state);
  else push(state, *pick, 0);
  return 1;
}

bool encode(lua_State *state, int index, std::string &out, const char *&error) {
  if (!encodeValue(state, index, 0, out, error)) return false;
  if (out.size() > MaxEncodeBytes) { error = "JSON too large"; return false; }
  return true;
}

}  // namespace AppLuaJson
