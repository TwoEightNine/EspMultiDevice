// SDK 7 additions to AppRuntime: `esp.apps.status(id)` and
// `esp.apps.open(id)`, which check and launch other SD apps by exact ID.
#include "AppRuntime.h"

#include "../../vendor_shims/lua/LuaConfig.h"

extern "C" {
#include "../../../third_party/lua/lua.h"
#include "../../../third_party/lua/lauxlib.h"
}

#include "../../services/apps/AppSettings.h"
#include "../../services/storage/SdCardController.h"
#include "AppPackage.h"

namespace {
int pushFailure(lua_State *state, bool nilFirst, const char *error) {
  if (nilFirst) lua_pushnil(state); else lua_pushboolean(state, false);
  lua_pushstring(state, error);
  return 2;
}

void setFunction(lua_State *state, AppRuntime *runtime, lua_CFunction function, const char *name) {
  lua_pushlightuserdata(state, runtime);
  lua_pushcclosure(state, function, 1);
  lua_setfield(state, -2, name);
}

// Reads argument 1 as an app ID; false with the failure pair pushed.
bool checkAppId(lua_State *state, bool nilFirst, String &id) {
  size_t length = 0;
  const char *value = luaL_checklstring(state, 1, &length);
  if (length > 32 || memchr(value, 0, length)) {
    pushFailure(state, nilFirst, "Invalid app ID");
    return false;
  }
  id = String(value);
  if (!AppPackage::validId(id)) {
    pushFailure(state, nilFirst, "Invalid app ID");
    return false;
  }
  return true;
}

// Errors that mean the card could not be read, so nothing is known about the
// package; anything else is a property of the package itself.
bool cardError(const String &error) {
  return error == "SD busy" || error == "SD card unavailable";
}
}  // namespace

void AppRuntime::registerSdk7() {
  lua_newtable(_lua);
  setFunction(_lua, this, luaAppsStatus, "status");
  setFunction(_lua, this, luaAppsOpen, "open");
  lua_setfield(_lua, -2, "apps");
}

int AppRuntime::luaAppsStatus(lua_State *state) {
  AppRuntime *runtime = self(state);
  String id;
  if (!checkAppId(state, true, id)) return 2;
  if (!runtime->_card.isMounted()) return pushFailure(state, true, "SD card unavailable");
  // Reading the manifest is bounded host work outside the callback limit.
  const int64_t started = callbackClockUs();
  const AppPackage package = AppPackage::load(runtime->_card, id);
  const bool enabled = runtime->_appSettings.isEnabled(id);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!package.valid && cardError(package.error))
    return pushFailure(state, true, package.error.c_str());

  lua_createtable(state, 0, 8);
  lua_pushboolean(state, package.installed);
  lua_setfield(state, -2, "installed");
  lua_pushboolean(state, package.valid);
  lua_setfield(state, -2, "valid");
  lua_pushboolean(state, enabled);
  lua_setfield(state, -2, "enabled");
  lua_pushboolean(state, package.valid && enabled && id != runtime->_id);
  lua_setfield(state, -2, "launchable");
  if (package.valid) {
    lua_pushstring(state, package.name.c_str());
    lua_setfield(state, -2, "name");
    lua_pushinteger(state, package.sdk);
    lua_setfield(state, -2, "sdk");
    lua_pushboolean(state, package.showInHome);
    lua_setfield(state, -2, "show_in_home");
  } else if (package.installed) {
    lua_pushstring(state, package.error.c_str());
    lua_setfield(state, -2, "error");
  }
  return 1;
}

int AppRuntime::luaAppsOpen(lua_State *state) {
  AppRuntime *runtime = self(state);
  String id;
  if (!checkAppId(state, false, id)) return 2;
  if (id == runtime->_id) return pushFailure(state, false, "Already running");
  if (runtime->_navigationPending) return pushFailure(state, false, "Navigation pending");
  if (!runtime->_card.isMounted()) return pushFailure(state, false, "SD card unavailable");
  const int64_t started = callbackClockUs();
  const AppPackage package = AppPackage::load(runtime->_card, id);
  const bool enabled = runtime->_appSettings.isEnabled(id);
  runtime->_callbackStartedUs += callbackClockUs() - started;
  if (!package.valid) {
    if (cardError(package.error)) return pushFailure(state, false, package.error.c_str());
    return pushFailure(state, false, package.installed ? "App invalid" : "App not found");
  }
  if (!enabled) return pushFailure(state, false, "App disabled");
  if (xSemaphoreTake(runtime->_mutex, pdMS_TO_TICKS(20)) != pdTRUE)
    return pushFailure(state, false, "Host busy");
  runtime->_navigation = Navigation{};
  runtime->_navigation.kind = NavigationKind::LaunchApp;
  snprintf(runtime->_navigation.appId, sizeof(runtime->_navigation.appId), "%s", id.c_str());
  runtime->_navigationPending = true;
  xSemaphoreGive(runtime->_mutex);
  Serial.printf("[SD app] %s: opening app %s\n", runtime->_id.c_str(), id.c_str());
  lua_pushboolean(state, true);
  return 1;
}
