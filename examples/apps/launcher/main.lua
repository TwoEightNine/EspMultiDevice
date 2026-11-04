-- SDK 7 example: checks other apps by ID with esp.apps.status and opens them
-- with esp.apps.open. "launcher_helper" is hidden from Home, so this app is
-- the only way to start it.
local targets = {"launcher_helper", "counter", "missing_app"}
local message = "Tap an app to open it."

local function describe(id)
  local status, err = esp.apps.status(id)
  if not status then return id .. ": " .. err end
  if not status.installed then return id .. ": not installed" end
  if not status.valid then return id .. ": invalid (" .. (status.error or "?") .. ")" end
  local parts = {status.name, "SDK " .. status.sdk}
  parts[#parts + 1] = status.enabled and "enabled" or "disabled"
  parts[#parts + 1] = status.show_in_home and "in Home" or "not in Home"
  return table.concat(parts, ", ")
end

local function show()
  local rows = {title = "Launcher"}
  for _, id in ipairs(targets) do
    rows[#rows + 1] = {kind = "text", text = describe(id)}
  end
  for _, id in ipairs(targets) do
    rows[#rows + 1] = {kind = "button", id = id, text = "Open " .. id}
  end
  rows[#rows + 1] = {kind = "text", text = message}
  esp.ui.show(rows)
end

return {
  on_start = show,
  on_action = function(id)
    local ok, err = esp.apps.open(id)
    -- On success this app ends after the callback and the target replaces it.
    message = ok and ("Opening " .. id .. "...") or ("Cannot open " .. id .. ": " .. err)
    show()
  end,
}
