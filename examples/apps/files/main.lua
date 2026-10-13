-- SDK 4 file manager: the built-in Files screen rebuilt on esp.fs for ordinary
-- (non-secret) storage. Browse folders, open media in the built-in viewers,
-- create folders, rename, move, and delete. Needs the "files" manifest
-- permission. Coordinates are relative to the area below the app bar.
local black, grey, lightgrey, white = 0, 16904, 54938, 65535
local red, cyan = 63488, 2047

local ROWS, ROW_TOP, ROW_H = 8, 32, 40
local STATUS_Y = 364
local footer = {
  {x = 16,  y = 384, w = 66, h = 40},
  {x = 90,  y = 384, w = 66, h = 40},
  {x = 164, y = 384, w = 66, h = 40},
  {x = 238, y = 384, w = 66, h = 40},
}
local menu = {
  open   = {x = 16, y = 80,  w = 288, h = 44},
  rename = {x = 16, y = 132, w = 288, h = 44},
  move   = {x = 16, y = 184, w = 288, h = 44},
  delete = {x = 16, y = 236, w = 288, h = 44},
  back   = {x = 16, y = 372, w = 288, h = 44},
}
local HOLD_MS = 1000

local view = "list"          -- "list", "menu", or "busy"
local dir = "/"
local loading = false
local count, page, rows = 0, 0, {}
local selected = nil         -- entry shown in the menu
local moving = nil           -- {path=..., name=...} while choosing a target
local pending = nil          -- what the open keyboard prompt is for
local hold_started = nil
local message, message_until = nil, 0
local busy_label = ""
local dirty = true

local function in_rect(x, y, r)
  return x >= r.x and x < r.x + r.w and y >= r.y and y < r.y + r.h
end

local function join(folder, name)
  return folder == "/" and "/" .. name or folder .. "/" .. name
end

local function parent(folder)
  local up = folder:match("^(.*)/[^/]+$")
  return (up and up ~= "") and up or "/"
end

local function is_continuation(byte)
  return byte and byte >= 0x80 and byte < 0xC0
end

-- Drawing text is limited to 96 bytes; cut on a UTF-8 boundary.
local function clip(text, max)
  if #text <= max then return text end
  local n = max - 3
  while n > 0 and is_continuation(text:byte(n + 1)) do n = n - 1 end
  return text:sub(1, n) .. "..."
end

local function clip_left(text, max)
  if #text <= max then return text end
  local first = #text - (max - 3) + 1
  while first <= #text and is_continuation(text:byte(first)) do first = first + 1 end
  return "..." .. text:sub(first)
end

local function size_text(bytes)
  if bytes < 1024 then return bytes .. " B" end
  if bytes < 1024 * 1024 then return string.format("%.1f KB", bytes / 1024) end
  return string.format("%.1f MB", bytes / (1024 * 1024))
end

local function notify(text)
  message = text
  message_until = esp.time.millis() + 2500
  dirty = true
end

local function page_count()
  return math.max(1, (count + ROWS - 1) // ROWS)
end

local function load_page()
  rows = esp.fs.entries(page * ROWS, ROWS) or {}
  dirty = true
end

local function scan(folder)
  local ok, err = esp.fs.scan(folder)
  view, selected, hold_started = "list", nil, nil
  rows, count, page = {}, 0, 0
  loading = ok
  if ok then dir = folder else notify(err) end
  dirty = true
end

local function start_job(label, ok, err)
  hold_started = nil
  if ok then
    view, busy_label = "busy", label
  else
    notify(err)
  end
  dirty = true
end

local function button(r, label, enabled, fill)
  esp.ui.button(r.x, r.y, r.w, r.h, label, enabled and (fill or grey) or black,
                enabled and white or grey, black)
end

local function render_list()
  esp.tft.begin_frame(black, "Lua Files")
  esp.tft.drawString(clip_left(dir, 40), 16, 16, lightgrey, black, 0, 3)
  for index, entry in ipairs(rows) do
    local icon = entry.dir and "folder" or
                 (entry.openable and entry.kind ~= "other") and entry.kind or "file"
    esp.ui.list_cell(ROW_TOP + (index - 1) * ROW_H, clip(entry.name, 96), icon, "more")
  end
  local status
  if message then status = message
  elseif loading then status = "Loading..."
  elseif moving then status = "Move: " .. moving.name
  elseif count == 0 then status = "Empty folder"
  else status = (page + 1) .. " / " .. page_count() end
  esp.tft.drawString(clip(status, 40), 160, STATUS_Y, message and cyan or lightgrey, black, 0, 4)
  button(footer[1], "Up", dir ~= "/")
  if moving then
    button(footer[2], "Here", not loading)
    button(footer[3], "Cancel", true)
  else
    button(footer[2], "New", not loading)
    button(footer[3], "<", page > 0)
  end
  button(footer[4], ">", page_count() > 1)
  esp.tft.present()
end

local function render_menu()
  local entry = selected
  esp.tft.begin_frame(black, "Lua Files")
  esp.tft.drawString(clip(entry.name, 40), 16, 20, white, black, 1, 3)
  local details = entry.dir and "Folder" or (entry.kind .. ", " .. size_text(entry.size))
  esp.tft.drawString(details, 16, 50, lightgrey, black, 0, 3)
  button(menu.open, "Open", entry.dir or entry.openable)
  button(menu.rename, "Rename", true)
  button(menu.move, "Move", true)
  button(menu.delete, "Hold to delete", true, red)
  local held = hold_started and esp.time.millis() - hold_started or 0
  esp.ui.progress(16, 288, 288, 8, math.min(100, held * 100 // HOLD_MS), red, grey, black)
  if message then esp.tft.drawString(clip(message, 40), 160, 330, cyan, black, 0, 4) end
  button(menu.back, "Back", true)
  esp.tft.present()
end

local function render_busy(done)
  esp.tft.begin_frame(black, "Lua Files")
  esp.tft.drawString(busy_label, 160, 150, white, black, 1, 4)
  esp.tft.drawString(done .. " done", 160, 184, lightgrey, black, 0, 4)
  button(menu.back, "Cancel", true)
  esp.tft.present()
end

local function open_entry(entry)
  local path = join(dir, entry.name)
  if entry.dir then scan(path) return end
  if not entry.openable then notify("Unsupported file") return end
  local ok, err = esp.fs.open(path)
  if not ok then notify(err) end
end

local function touch_list(x, y)
  if in_rect(x, y, footer[1]) then
    if dir ~= "/" then scan(parent(dir)) end
  elseif in_rect(x, y, footer[2]) then
    if loading then return end
    if moving then
      local target = join(dir, moving.name)
      local source = moving.path
      moving = nil
      start_job("Moving...", esp.fs.rename(source, target))
    else
      pending = {kind = "mkdir"}
      esp.ui.prompt("New folder")
    end
  elseif in_rect(x, y, footer[3]) then
    if moving then moving = nil dirty = true
    elseif page > 0 then page = page - 1 load_page() end
  elseif in_rect(x, y, footer[4]) then
    if page_count() > 1 then page = (page + 1) % page_count() load_page() end
  elseif y >= ROW_TOP and y < ROW_TOP + ROWS * ROW_H then
    local entry = rows[(y - ROW_TOP) // ROW_H + 1]
    if not entry then return end
    -- The trailing 40x40 cell opens the actions; the rest of the row opens it.
    if x >= 320 - ROW_H and not moving then
      view, selected, message = "menu", entry, nil
      dirty = true
    else
      open_entry(entry)
    end
  end
end

local function touch_menu(x, y)
  local entry = selected
  if in_rect(x, y, menu.back) then
    view, selected = "list", nil
  elseif in_rect(x, y, menu.open) then
    view, selected = "list", nil
    open_entry(entry)
  elseif in_rect(x, y, menu.rename) then
    pending = {kind = "rename", name = entry.name}
    esp.ui.prompt("Rename", clip(entry.name, 255))
  elseif in_rect(x, y, menu.move) then
    moving = {path = join(dir, entry.name), name = entry.name}
    view, selected = "list", nil
  elseif in_rect(x, y, menu.delete) then
    hold_started = esp.time.millis()
  end
  dirty = true
end

local function on_start()
  scan("/")
end

local function on_touch(x, y)
  if view == "list" then touch_list(x, y)
  elseif view == "menu" then touch_menu(x, y)
  elseif in_rect(x, y, menu.back) then esp.fs.cancel() end
end

local function on_touch_end()
  if hold_started then hold_started = nil dirty = true end
end

local function on_input(text)
  local request = pending
  pending = nil
  if not request or not text or text == "" then return end
  if text:find("/", 1, true) then notify("Invalid name") return end
  if request.kind == "mkdir" then
    local ok, err = esp.fs.mkdir(join(dir, text))
    if ok then scan(dir) else notify(err) end
  elseif text ~= request.name then
    start_job("Renaming...", esp.fs.rename(join(dir, request.name), join(dir, text)))
  end
end

local function on_resume()
  dirty = true
end

local function on_tick(now)
  if message and now >= message_until then message = nil dirty = true end
  if view == "busy" then
    local job = esp.fs.job()
    if job.active then
      render_busy(job.done)
      return
    end
    scan(dir)
    notify(job.error or "Done")
  elseif loading then
    local status = esp.fs.scan_status()
    if status.ready then
      loading, count = false, status.count
      if status.truncated then notify("Showing first " .. count) end
      load_page()
    elseif not status.scanning then
      loading = false
      notify(status.error or "Unable to list folder")
    end
  end
  if view == "menu" and hold_started then
    if now - hold_started >= HOLD_MS then
      start_job("Deleting...", esp.fs.remove(join(dir, selected.name)))
      if view ~= "busy" then view, selected = "list", nil end
    end
    dirty = true
  end
  if not dirty then return end
  dirty = false
  if view == "menu" then render_menu()
  elseif view == "busy" then render_busy(0)
  else render_list() end
end

return {
  on_start = on_start,
  on_touch = on_touch,
  on_touch_end = on_touch_end,
  on_input = on_input,
  on_resume = on_resume,
  on_tick = on_tick,
}
