#pragma once

#include <stdint.h>

#include <string>

struct lua_State;
class JsonPick;

// Conversions between JsonPick results, Lua values, and JSON text for the
// SDK 6 `esp.json` API. No Arduino dependency, so the host app harness uses
// the same code as the firmware.
namespace AppLuaJson {

// The light userdata that stands for JSON null (`esp.json.null`).
void *nullValue();
// Pushes the kept value at `index` and returns the index after it and its
// descendants. Raises a Lua error when the Lua heap or stack runs out.
uint32_t push(lua_State *state, const JsonPick &pick, uint32_t index);
// lua_CFunction for a protected call: argument 1 is a light userdata
// JsonPick; pushes its root value, or nil for an empty result.
int pushTree(lua_State *state);
// Encodes the value at `index`: a sequence becomes an array, any other table
// an object with sorted string keys (an empty table is `{}`). Depth 16 and
// 16 KiB of output at most.
bool encode(lua_State *state, int index, std::string &out, const char *&error);

}  // namespace AppLuaJson
