-- SDK 5 image viewer: the built-in Image Viewer rebuilt on the host image
-- layer (esp.image) for ordinary (non-secret) storage. A folder picker lists
-- folders and images; the viewer has the informative/full-screen toggle, 48px
-- edge navigation with wrap, and crop zoom. Needs the "files" manifest
-- permission. Coordinates are relative to the area below the app bar.
local black, grey, lightgrey, white, cyan = 0, 16904, 54938, 65535, 2047

local ROWS, ROW_TOP, ROW_H = 8, 32, 40
local STATUS_Y = 364
local footer = {
  {x = 16,  y = 384, w = 90, h = 40},
  {x = 115, y = 384, w = 90, h = 40},
  {x = 214, y = 384, w = 90, h = 40},
}
local EDGE = 48              -- previous/next tap strips, as in the built-in viewer
local WIDTH = 320
local PAGES_PER_TICK = 4     -- 16-entry listing pages indexed per tick
local LAST_DIR = "last_dir"

local view = "list"          -- "list" or "image"
local dir = "/"
local loading = false
local total, indexed = 0, 0  -- entries in the host listing, and how many are read
local names = {}             -- folders first, then openable images
local folders = 0
local page = 0
local current = nil          -- index into names of the image being shown
local bar = true
local message, message_until = nil, 0
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

local function notify(text)
  message = text
  message_until = esp.time.millis() + 2500
  dirty = true
end

local function page_count()
  return math.max(1, (#names + ROWS - 1) // ROWS)
end

local function scan(folder)
  local ok, err = esp.fs.scan(folder)
  if not ok then notify(err) return false end
  dir, loading = folder, true
  names, folders, total, indexed, page = {}, 0, 0, 0, 0
  dirty = true
  return true
end

-- Copies folders and openable images out of the host listing a few pages at
-- a time, so a large folder never exceeds the callback limits.
local function index_listing()
  for _ = 1, PAGES_PER_TICK do
    if indexed >= total then break end
    local entries = esp.fs.entries(indexed, 16)
    if not entries or #entries == 0 then indexed = total break end
    for _, entry in ipairs(entries) do
      if entry.dir then
        folders = folders + 1
        names[#names + 1] = entry.name
      elseif entry.kind == "image" and entry.openable then
        names[#names + 1] = entry.name
      end
    end
    indexed = indexed + #entries
  end
  if indexed >= total then
    loading = false
    esp.storage.write(LAST_DIR, dir)
  end
  dirty = true
end

local function button(r, label, enabled)
  esp.ui.button(r.x, r.y, r.w, r.h, label, enabled and grey or black,
                enabled and white or grey, black)
end

local function render_list()
  esp.tft.begin_frame(black, "Lua Images")
  esp.tft.drawString(clip_left(dir, 40), 16, 16, lightgrey, black, 0, 3)
  for row = 1, ROWS do
    local index = page * ROWS + row
    local name = names[index]
    if not name then break end
    esp.ui.list_cell(ROW_TOP + (row - 1) * ROW_H, clip(name, 96),
                     index <= folders and "folder" or "image")
  end
  local status
  if message then status = message
  elseif loading then status = "Loading... " .. indexed .. " / " .. total
  elseif #names == 0 then status = "No images"
  else status = (page + 1) .. " / " .. page_count() end
  esp.tft.drawString(clip(status, 40), 160, STATUS_Y, message and cyan or lightgrey, black, 0, 4)
  button(footer[1], "Up", dir ~= "/")
  button(footer[2], "<", page > 0)
  button(footer[3], ">", page_count() > 1)
  esp.tft.present()
end

local function show(index)
  local ok, err = esp.image.open(join(dir, names[index]))
  if not ok then notify(err) return false end
  current = index
  return true
end

-- Moves to the neighbouring image, wrapping at either end of the folder.
-- Like the built-in viewer this keeps the bar state and starts unzoomed.
local function step(delta)
  local images = #names - folders
  if images < 2 then return end
  local position = (current - folders - 1 + delta) % images
  show(folders + 1 + position)
end

local function close_image()
  esp.image.close()
  view, bar = "list", true
  if current then page = (current - 1) // ROWS end
  current = nil
  dirty = true
end

local function touch_list(x, y)
  if in_rect(x, y, footer[1]) then
    if dir ~= "/" then scan(parent(dir)) end
  elseif in_rect(x, y, footer[2]) then
    if page > 0 then page = page - 1 dirty = true end
  elseif in_rect(x, y, footer[3]) then
    if page_count() > 1 then page = (page + 1) % page_count() dirty = true end
  elseif y >= ROW_TOP and y < ROW_TOP + ROWS * ROW_H then
    local index = page * ROWS + (y - ROW_TOP) // ROW_H + 1
    local name = names[index]
    if not name then return end
    if index <= folders then
      scan(join(dir, name))
    else
      bar = true
      esp.image.set_bar(true)
      if show(index) then view = "image" end
    end
  end
end

local function touch_image(x)
  if x < EDGE then
    step(-1)
  elseif x >= WIDTH - EDGE then
    step(1)
  else
    -- An image that failed to load keeps the toggle, so a hidden bar and its
    -- Close target can always be brought back.
    local state = esp.image.status().state
    if state == "ready" or state == "error" then
      bar = not bar
      esp.image.set_bar(bar)
    end
  end
end

local function on_start()
  local saved = esp.storage.read(LAST_DIR)
  if not (saved and saved ~= "" and scan(saved)) then scan("/") end
end

local function on_touch(x, y)
  if view == "image" then touch_image(x) else touch_list(x, y) end
end

-- The host bar over an image reports its Close and Zoom targets here.
local function on_action(id)
  if view ~= "image" then return end
  if id == "image.close" then
    close_image()
  elseif id == "image.zoom" then
    local status = esp.image.status()
    if status.zoom_available then esp.image.set_zoom(not status.zoom) end
  end
end

local function on_resume()
  -- The host drops the image layer whenever another screen covers the app.
  if view == "image" and esp.image.status().state == "none" then close_image() end
  dirty = true
end

local function on_tick(now)
  if message and now >= message_until then message = nil dirty = true end
  if loading then
    if indexed < total then
      index_listing()
    else
      local status = esp.fs.scan_status()
      if status.ready then
        total = status.count
        if status.truncated then notify("Showing first " .. total) end
        index_listing()
      elseif not status.scanning then
        loading = false
        notify(status.error or "Unable to list folder")
      end
    end
  end
  if view ~= "list" or not dirty then return end
  dirty = false
  render_list()
end

return {
  on_start = on_start,
  on_touch = on_touch,
  on_action = on_action,
  on_resume = on_resume,
  on_tick = on_tick,
}
