-- SDK 6 reader for the 2ch (Двач) imageboard: open a board by its exact name,
-- page through its threads, read a thread, and reply after solving the site's
-- emoji captcha. Needs the "network" permission and the hosts listed in the
-- manifest. Coordinates are relative to the area below the app bar.
local black, white, grey, lightgrey = 0, 65535, 33808, 50712
local dark, cell, accent, red, cyan = 8452, 12678, 64800, 63488, 2047

-- Mirrors in fallback order. An optional app-data config.json such as
-- {"mirrors": ["http://192.168.1.20:8080"]} replaces them (for the mock
-- server in tools/mock_dvach.py); the manifest's hosts still apply.
local MIRRORS = {"https://2ch.su", "https://2ch.hk", "https://2ch.org"}
local ROW_H, ROWS = 64, 6
local LINE_H, LINES = 21, 18
local FOOT_Y = 388
local MAX_RECENT = 8
local HOLD_MS = 800

-- Footer targets shared by the board and thread pages.
local foot = {
  back = {x = 8, y = FOOT_Y, w = 64, h = 40},
  prev = {x = 80, y = FOOT_Y, w = 48, h = 40},
  mid = {x = 136, y = FOOT_Y, w = 104, h = 40},
  next = {x = 248, y = FOOT_Y, w = 64, h = 40},
}
local home_open = {x = 16, y = 8, w = 288, h = 44}
local captcha_cancel = {x = 8, y = FOOT_Y, w = 120, h = 40}
local captcha_reload = {x = 136, y = FOOT_Y, w = 176, h = 40}

-- `mirror` is the index into MIRRORS; settings.json stores its base URL.
local settings = {mirror = 1, boards = {}}
local cookies = {}
local pending = {}          -- request id -> {handler, ...}
local loading = nil         -- {id, label} of the request the page waits for
local message, message_until = nil, 0
local dirty = true
local touch = nil           -- {x, y, at} of the touch being held

local page = "home"         -- "home", "board", "thread", "captcha", "sending"
-- Board rows are compact arrays: {num, title, posts, files, lasthit, flags}
-- with flags 1 = pinned and 2 = closed. Thread lines are two parallel arrays
-- (text, and true for post headers); `offset` is the first line on screen.
local board = nil           -- {id, name, threads, page}
local thread = nil          -- {num, title, text, header, offset, next_from, done}
local MAX_LINES = 600
local captcha = nil         -- {id, challenge, image, keys, comment}

-- Large responses are turned into rows a slice at a time from on_tick: on
-- the device the Lua heap is in PSRAM, and converting a whole 250-thread
-- catalog in one callback overruns the 20 ms limit.
local queue = nil           -- {items, index, slice, each, done}

-- Text --------------------------------------------------------------------

local function is_continuation(byte)
  return byte and byte >= 0x80 and byte < 0xC0
end

-- Canvas text is limited to 96 bytes; cut on a UTF-8 boundary.
local function clip(text, max)
  max = max or 96
  if #text <= max then return text end
  local n = max - 3
  while n > 0 and is_continuation(text:byte(n + 1)) do n = n - 1 end
  return text:sub(1, n) .. "..."
end

local function plain(html, max)
  local text = esp.text.from_html(html or "", max or 200)
  return (text:gsub("%s+", " "))
end

local function ago(epoch)
  local now = esp.device.status().epoch_seconds
  if now == 0 or not epoch or epoch == 0 then return "" end
  local s = math.max(0, now - epoch)
  if s < 60 then return "now" end
  if s < 3600 then return (s // 60) .. "m" end
  if s < 86400 then return (s // 3600) .. "h" end
  return (s // 86400) .. "d"
end

local function notify(text)
  message = text
  message_until = esp.time.millis() + 3000
  dirty = true
end

local function in_rect(x, y, r)
  return x >= r.x and x < r.x + r.w and y >= r.y and y < r.y + r.h
end

-- Settings ----------------------------------------------------------------

local function save_settings()
  local text = esp.json.encode({mirror = MIRRORS[settings.mirror], boards = settings.boards})
  if text then esp.storage.write("settings.json", text) end
end

local function load_config()
  local text = esp.storage.read("config.json")
  local config = text and esp.json.decode(text)
  if type(config) ~= "table" or type(config.mirrors) ~= "table" then return end
  local mirrors = {}
  for _, base in ipairs(config.mirrors) do
    if type(base) == "string" and base:match("^https?://[%w%.%-]+:?%d*$") then
      mirrors[#mirrors + 1] = base
    end
  end
  if #mirrors > 0 then MIRRORS = mirrors end
end

local function load_settings()
  local text = esp.storage.read("settings.json")
  if not text then return end
  local saved = esp.json.decode(text)
  if type(saved) ~= "table" then return end
  for i, base in ipairs(MIRRORS) do
    if saved.mirror == base then settings.mirror = i end
  end
  if type(saved.boards) == "table" then
    for _, entry in ipairs(saved.boards) do
      if type(entry) == "table" and type(entry.id) == "string" and #settings.boards < MAX_RECENT then
        settings.boards[#settings.boards + 1] = {id = entry.id, name = tostring(entry.name or "")}
      end
    end
  end
end

local function remember_board(id, name)
  for i = #settings.boards, 1, -1 do
    if settings.boards[i].id == id then table.remove(settings.boards, i) end
  end
  table.insert(settings.boards, 1, {id = id, name = name or ""})
  while #settings.boards > MAX_RECENT do table.remove(settings.boards) end
  save_settings()
end

-- Network -----------------------------------------------------------------

local function base_url()
  if not MIRRORS[settings.mirror] then settings.mirror = 1 end
  return MIRRORS[settings.mirror]
end

local function cookie_header()
  local parts = {}
  for name, value in pairs(cookies) do parts[#parts + 1] = name .. "=" .. value end
  table.sort(parts)
  return #parts > 0 and table.concat(parts, "; ") or nil
end

local function absorb_cookies(headers)
  local raw = headers and headers["set-cookie"]
  if not raw then return end
  for line in raw:gmatch("[^\n]+") do
    local name, value = line:match("^%s*([^=;%s]+)=([^;]*)")
    if name and #name <= 64 and #value <= 512 then cookies[name] = value end
  end
end

local function challenged(r)
  return (r.status == 403 or r.status == 503) and r.headers and
         (r.headers["cf-mitigated"] or ""):find("challenge") ~= nil
end

local unreachable = {
  ["Cannot connect"] = true, ["TLS failed"] = true,
  ["Timeout"] = true, ["DNS lookup failed"] = true,
}

-- Sends `opts` to the current mirror. A GET that cannot reach a mirror, or
-- meets a browser check there, moves on to the next mirror once.
local function send(path, opts, handler, label, tried)
  opts.url = base_url() .. path
  local headers = opts.headers or {}
  headers.Cookie = cookie_header()
  opts.headers = headers
  local id, err = esp.net.request(opts)
  if not id then
    handler(nil, err)
    return
  end
  pending[id] = {handler = handler, path = path, opts = opts, label = label,
                 tried = tried or 1}
  if label then loading = {id = id, label = label} end
  dirty = true
end

-- Page helpers ------------------------------------------------------------

local function free_captcha()
  if captcha then
    if captcha.image then esp.bitmap.free(captcha.image) end
    for _, key in ipairs(captcha.keys or {}) do esp.bitmap.free(key) end
  end
  captcha = nil
end

local function cancel_loading()
  if loading then
    esp.net.cancel(loading.id)
    pending[loading.id] = nil
    loading = nil
  end
end

local function go(name)
  page = name
  touch = nil
  queue = nil
  dirty = true
end

local function run_slice()
  local q = queue
  if not q then return end
  local last = math.min(#q.items, q.index + q.slice - 1)
  for i = q.index, last do q.each(q.items[i]) end
  q.index = last + 1
  dirty = true
  if q.index > #q.items then
    queue = nil
    if q.done then q.done() end
  end
end

local function process(items, slice, each, done)
  queue = {items = items, index = 1, slice = slice, each = each, done = done}
  run_slice()
end

-- Board -------------------------------------------------------------------

local function thread_title(t)
  local subject = type(t.subject) == "string" and t.subject or ""
  -- Most subjects are plain text; only markup needs the HTML converter.
  local title = subject:find("[&<]") and plain(subject, 200) or (subject:gsub("%s+", " "))
  if title == "" or title == " " then title = plain(t.comment, 200) end
  if title == "" then title = "#" .. tostring(t.num) end
  return clip(title, 96)
end

local function board_pages()
  return math.max(1, (#board.threads + ROWS - 1) // ROWS)
end

local function load_board(id)
  board = {id = id, name = "", threads = {}, page = 0}
  go("board")
  send("/" .. id .. "/catalog.json", {
    method = "GET", redirects = false, timeout_ms = 30000,
    json = {keep = {"board", "name", "threads", "num", "subject", "comment",
                    "posts_count", "files_count", "lasthit", "sticky", "closed"},
            max_string = 160, max_items = 250},
  }, function(r, err)
    if not r then board.error = err notify(err) return end
    if r.status == 404 then
      notify("No board /" .. id .. "/")
      go("home")
      return
    end
    if not r.ok or type(r.json) ~= "table" then
      board.error = r.error or ("HTTP " .. r.status)
      notify(board.error)
      return
    end
    board.error = nil
    local data = r.json
    board.name = type(data.board) == "table" and tostring(data.board.name or "") or ""
    board.threads = {}
    board.page = 0
    local rows = board.threads
    process(type(data.threads) == "table" and data.threads or {}, 25, function(t)
      local flags = 0
      if t.sticky and t.sticky ~= 0 and t.sticky ~= false then flags = flags | 1 end
      if t.closed and t.closed ~= 0 and t.closed ~= false then flags = flags | 2 end
      rows[#rows + 1] = {t.num, thread_title(t), t.posts_count or 0, t.files_count or 0,
                         t.lasthit or 0, flags}
    end, function()
      remember_board(id, board.name)
      if #rows == 0 then notify("Board is empty") end
    end)
  end, "Loading /" .. id .. "/")
end

local function open_board_prompt()
  esp.ui.prompt("Board name", "")
end

local function board_from_input(text)
  local id = (text or ""):lower():gsub("%s", ""):gsub("^/+", ""):gsub("/+$", "")
  if id:match("^[a-z0-9]+$") and #id <= 16 then return id end
  return nil
end

-- Thread ------------------------------------------------------------------

local POST_KEEP = {"posts", "num", "number", "date", "name", "comment", "trip", "op",
                   "result", "error", "message"}
local BATCH = 25

local function add_line(text, header)
  local n = #thread.text + 1
  thread.text[n] = text
  thread.header[n] = header
end

local function append_post(p)
  local header = "#" .. tostring(p.number or "?") .. "  " .. tostring(p.num) ..
                 "  " .. clip(tostring(p.date or ""), 40)
  if p.name and p.name ~= "" and p.name ~= "Аноним" then
    header = header .. "  " .. clip(plain(p.name, 60), 30)
  end
  add_line(clip(header), true)
  local body = esp.text.from_html(p.comment or "", 4096)
  for _, line in ipairs((esp.ui.wrap(body, 304, 1, 200))) do add_line(line, false) end
  add_line("", false)
end

local function load_posts(from, keep_offset)
  local num = thread.num
  thread.error = nil
  send("/api/mobile/v2/after/" .. board.id .. "/" .. num .. "/" .. from, {
    method = "GET", timeout_ms = 30000,
    json = {keep = POST_KEEP, max_string = 3000, max_items = BATCH},
  }, function(r, err)
    if not r then thread.error = err notify(err) return end
    if not r.ok or type(r.json) ~= "table" then
      thread.error = r.error or ("HTTP " .. r.status)
      notify(thread.error)
      return
    end
    if type(r.json.error) == "table" and r.json.result ~= 1 then
      thread.error = clip(tostring(r.json.error.message or "Thread unavailable"), 90)
      notify(thread.error)
      return
    end
    local posts = type(r.json.posts) == "table" and r.json.posts or {}
    if #posts == 0 then
      thread.done = true
      notify("No more posts")
      return
    end
    if not keep_offset then thread.offset = #thread.text end
    thread.done = #posts < BATCH
    process(posts, 4, function(p)
      append_post(p)
      thread.next_from = math.max(thread.next_from, (p.num or 0) + 1)
    end, function()
      -- Keep memory bounded: drop lines far above the current page.
      local total = #thread.text
      if total > MAX_LINES then
        local drop = total - MAX_LINES
        table.move(thread.text, drop + 1, total, 1)
        table.move(thread.header, drop + 1, total, 1)
        for i = total, total - drop + 1, -1 do
          thread.text[i] = nil
          thread.header[i] = nil
        end
        thread.offset = math.max(0, thread.offset - drop)
      end
    end)
  end, "Loading posts")
end

local function open_thread(row)
  thread = {num = row[1], title = row[2], text = {}, header = {}, offset = 0,
            next_from = row[1], done = false}
  go("thread")
  load_posts(row[1], true)
end

-- Captcha and posting -----------------------------------------------------

-- The id response carries the proof-of-work challenge either as an object or
-- as a Python-style string such as "{'hash': '…', 'limit': 20000, …}".
local function parse_challenge(value)
  if type(value) == "table" then
    return value.hash, tonumber(value.limit), value.template
  end
  if type(value) ~= "string" then return nil end
  return value:match("'hash':%s*'([0-9a-f]+)'"),
         tonumber(value:match("'limit':%s*(%d+)")),
         value:match("'template':%s*'([^']*)'")
end

local function decode_bitmap(base64, background)
  if type(base64) ~= "string" then return nil end
  local bytes = esp.base64.decode(base64)
  if not bytes then return nil end
  return esp.bitmap.decode(bytes, background)
end

local function show_captcha_step(data)
  if captcha.image then esp.bitmap.free(captcha.image) end
  for _, key in ipairs(captcha.keys or {}) do esp.bitmap.free(key) end
  captcha.image, captcha.keys = nil, {}
  local image, err = decode_bitmap(data.image, black)
  if not image then notify(err or "Bad captcha image") return false end
  captcha.image = image
  for i, key in ipairs(type(data.keyboard) == "table" and data.keyboard or {}) do
    if i > 8 then break end
    captcha.keys[i] = decode_bitmap(key, cell) or false
  end
  captcha.busy = false
  dirty = true
  return true
end

local start_captcha

local function submit_post(token)
  go("sending")
  local hash, limit, template = parse_challenge(captcha.challenge)
  local nonce = nil
  if hash and limit and template and #hash == 128 and template:find("%%d") then
    nonce = esp.hash.find_sha512(template, hash, math.min(limit, 200000))
  end
  local parts = {
    {name = "task", value = "post"},
    {name = "board", value = board.id},
    {name = "thread", value = tostring(thread.num)},
    {name = "captcha_type", value = "emoji_captcha"},
    {name = "email", value = ""},
    {name = "comment", value = captcha.comment},
    {name = "emoji_captcha_id", value = token},
  }
  if nonce then parts[#parts + 1] = {name = "2ch_challenge", value = tostring(nonce)} end
  local comment = captcha.comment
  free_captcha()
  send("/user/posting?nc=1", {
    method = "POST", multipart = parts, timeout_ms = 30000,
    json = {keep = {"result", "num", "thread", "error", "code", "message", "Status", "Reason"},
            max_string = 300},
  }, function(r, err)
    go("thread")
    if not r then notify(err) return end
    local data = type(r.json) == "table" and r.json or {}
    if data.result == 1 or data.Status == "OK" then
      notify("Posted #" .. tostring(data.num or ""))
      if not thread.done then return end
      load_posts(thread.next_from, false)
      return
    end
    local reason = type(data.error) == "table" and data.error.message or data.Reason
    notify(clip("Not posted: " .. tostring(reason or r.error or ("HTTP " .. r.status)), 90))
    -- Keep the text so a retry does not need retyping.
    thread.draft = comment
  end, "Sending")
end

local function click_emoji(index)
  if captcha.busy then return end
  captcha.busy = true
  local body = esp.json.encode({captchaTokenID = captcha.id, emojiNumber = index})
  send("/api/captcha/emoji/click", {
    method = "POST", body = body,
    headers = {["Content-Type"] = "application/json"},
    json = {keep = {"image", "keyboard", "success", "result", "error"}, max_string = 65536},
  }, function(r, err)
    if not captcha then return end
    captcha.busy = false
    if not r then notify(err) return end
    local data = type(r.json) == "table" and r.json or {}
    if type(data.success) == "string" then
      submit_post(data.success)
    elseif data.image and data.keyboard then
      show_captcha_step(data)
    else
      notify("Wrong emoji, new captcha")
      start_captcha(captcha.comment)
    end
  end, "Checking")
end

start_captcha = function(comment)
  free_captcha()
  captcha = {comment = comment, keys = {}, busy = true}
  go("captcha")
  send("/api/captcha/emoji/id?board=" .. board.id .. "&thread=" .. tostring(thread.num), {
    method = "GET",
    json = {keep = {"result", "type", "id", "challenge", "warning", "banned",
                    "hash", "limit", "template"}, max_string = 2048},
  }, function(r, err)
    if not captcha then return end
    if not r then notify(err) go("thread") return end
    local data = type(r.json) == "table" and r.json or {}
    if data.result ~= 1 or type(data.id) ~= "string" then
      notify(clip(tostring(data.warning or data.banned or r.error or "Captcha unavailable"), 90))
      go("thread")
      return
    end
    captcha.id = data.id
    captcha.challenge = data.challenge
    send("/api/captcha/emoji/show?id=" .. esp.url.encode(data.id), {
      method = "GET", json = {keep = {"image", "keyboard"}, max_string = 65536},
    }, function(shown, show_err)
      if not captcha then return end
      if not shown then notify(show_err) go("thread") return end
      if type(shown.json) ~= "table" or not show_captcha_step(shown.json) then
        notify(shown.error or "Captcha unavailable")
        go("thread")
      end
    end, "Loading captcha")
  end, "Loading captcha")
end

-- Rendering ---------------------------------------------------------------

local function button(r, label, enabled, fill)
  esp.ui.button(r.x, r.y, r.w, r.h, label, enabled and (fill or grey) or dark,
                enabled and white or grey, black)
end

local function draw_loading()
  if not loading then return end
  local st = esp.net.status(loading.id)
  local received = st and st.received or 0
  esp.tft.fillRect(0, 300, 320, 80, black)
  esp.tft.drawString(clip(loading.label, 60), 160, 316, lightgrey, black, 1, 4)
  local kb = received // 1024
  local percent = (st and st.total > 0) and (received * 100 // st.total) or ((kb // 8) % 101)
  esp.ui.progress(40, 340, 240, 12, percent, accent, grey, black)
  esp.tft.drawString(kb .. " KB", 160, 366, grey, black, 0, 4)
end

local function draw_message()
  if message and esp.time.millis() < message_until then
    esp.tft.fillRect(0, 344, 320, 40, dark)
    esp.ui.text_box(8, 346, 304, 2, clip(message, 96), white, dark, 0)
  end
end

local function render_home()
  esp.tft.begin_frame(black, "Двач")
  button(home_open, "Open board...", true, accent)
  esp.tft.drawString("Recent boards (hold to remove)", 16, 64, grey, black, 0, 0)
  for i, entry in ipairs(settings.boards) do
    local label = "/" .. entry.id .. "/"
    if entry.name ~= "" then label = label .. "  " .. entry.name end
    esp.ui.list_cell(84 + (i - 1) * 40, clip(label, 96), "folder")
  end
  if #settings.boards == 0 then
    esp.tft.drawString("Type a name such as pr, b, or po", 16, 96, lightgrey, black, 1, 0)
  end
  draw_message()
  esp.tft.present()
end

local function render_board()
  local title = "/" .. board.id .. "/"
  if board.name ~= "" then title = title .. " " .. board.name end
  esp.tft.begin_frame(black, clip(title, 40))
  local first = board.page * ROWS
  for i = 1, ROWS do
    local t = board.threads[first + i]
    if not t then break end
    local y = (i - 1) * ROW_H
    local pinned, closed = t[6] & 1 ~= 0, t[6] & 2 ~= 0
    esp.ui.text_box(8, y + 4, 304, 2, t[2], pinned and accent or white, black, 1)
    local meta = t[3] .. " replies  " .. t[4] .. " files"
    local when = ago(t[5])
    if when ~= "" then meta = meta .. "  " .. when end
    if pinned then meta = meta .. "  pinned" end
    if closed then meta = meta .. "  closed" end
    esp.tft.drawString(meta, 8, y + 46, grey, black, 0, 0)
    esp.tft.drawFastHLine(0, y + ROW_H - 1, 320, dark)
  end
  local pages = board_pages()
  local failed = board.error and #board.threads == 0 and not loading
  if failed then
    esp.ui.text_box(16, 120, 288, 3, clip("Couldn't load: " .. board.error), lightgrey, black, 1)
  end
  button(foot.back, "Back", true)
  button(foot.prev, "<", board.page > 0)
  button(foot.mid, failed and "Retry" or ((board.page + 1) .. "/" .. pages), not loading,
         failed and accent or dark)
  button(foot.next, ">", board.page + 1 < pages)
  draw_loading()
  draw_message()
  esp.tft.present()
end

local function render_thread()
  esp.tft.begin_frame(black, clip(thread.title, 40))
  for i = 1, LINES do
    local text = thread.text[thread.offset + i]
    if not text then break end
    if text ~= "" then
      local header = thread.header[thread.offset + i]
      esp.tft.drawString(text, 8, (i - 1) * LINE_H + 4, header and cyan or white, black,
                         header and 0 or 1, 0)
    end
  end
  local at_end = thread.offset + LINES >= #thread.text
  local failed = thread.error and at_end and not loading
  if failed and #thread.text == 0 then
    esp.ui.text_box(16, 120, 288, 3, clip("Couldn't load: " .. thread.error), lightgrey, black, 1)
  end
  button(foot.back, "Back", true)
  button(foot.prev, "<", thread.offset > 0)
  button(foot.mid, "Reply", not loading, accent)
  button(foot.next, failed and "Retry" or (at_end and "More" or ">"),
         not (at_end and (thread.done or loading or queue)))
  draw_loading()
  draw_message()
  esp.tft.present()
end

local function key_rect(i)
  local col, row = (i - 1) % 4, (i - 1) // 4
  return {x = 4 + col * 79, y = 156 + row * 76, w = 75, h = 70}
end

local function render_captcha()
  esp.tft.begin_frame(black, "Captcha")
  if captcha and captcha.image then
    esp.ui.bitmap(captcha.image, 10, 8)
    esp.tft.drawString("Tap the emoji shown above, in order", 160, 128, lightgrey, black, 0, 4)
    for i = 1, 8 do
      local r = key_rect(i)
      esp.tft.fillSmoothRoundRect(r.x, r.y, r.w, r.h, 8, cell, black)
      local key = captcha.keys[i]
      if key then esp.ui.bitmap(key, r.x + 12, r.y + 12) end
    end
  end
  button(captcha_cancel, "Cancel", true)
  button(captcha_reload, "New captcha", not (captcha and captcha.busy))
  draw_loading()
  draw_message()
  esp.tft.present()
end

local function render_sending()
  esp.tft.begin_frame(black, "Sending")
  esp.tft.drawString("Solving the challenge and posting...", 160, 160, lightgrey, black, 1, 4)
  draw_loading()
  draw_message()
  esp.tft.present()
end

local function render()
  dirty = false
  if page == "home" then render_home()
  elseif page == "board" then render_board()
  elseif page == "thread" then render_thread()
  elseif page == "captcha" then render_captcha()
  else render_sending() end
end

-- Input -------------------------------------------------------------------

local prompt_for = nil

local function tap_home(x, y)
  if in_rect(x, y, home_open) then
    prompt_for = "board"
    open_board_prompt()
  end
end

local function tap_board(x, y)
  if in_rect(x, y, foot.back) then
    cancel_loading()
    go("home")
  elseif in_rect(x, y, foot.prev) and board.page > 0 then
    board.page = board.page - 1
  elseif in_rect(x, y, foot.next) and board.page + 1 < board_pages() then
    board.page = board.page + 1
  elseif in_rect(x, y, foot.mid) and not loading then
    load_board(board.id)
  elseif y < ROWS * ROW_H and not loading then
    local t = board.threads[board.page * ROWS + y // ROW_H + 1]
    if t then open_thread(t) end
  end
  dirty = true
end

local function tap_thread(x, y)
  local at_end = thread.offset + LINES >= #thread.text
  if in_rect(x, y, foot.back) then
    cancel_loading()
    thread = nil
    go("board")
  elseif in_rect(x, y, foot.prev) then
    thread.offset = math.max(0, thread.offset - (LINES - 2))
  elseif in_rect(x, y, foot.next) then
    if not at_end then
      thread.offset = math.min(#thread.text - 1, thread.offset + (LINES - 2))
    elseif not thread.done and not loading and not queue then
      load_posts(thread.next_from, false)
    end
  elseif in_rect(x, y, foot.mid) and not loading then
    prompt_for = "reply"
    esp.ui.prompt("Reply to #" .. tostring(thread.num), thread.draft or "")
  end
  dirty = true
end

local function tap_captcha(x, y)
  if in_rect(x, y, captcha_cancel) then
    cancel_loading()
    local comment = captcha and captcha.comment
    free_captcha()
    thread.draft = comment
    go("thread")
  elseif in_rect(x, y, captcha_reload) and captcha and not captcha.busy then
    start_captcha(captcha.comment)
  elseif captcha and captcha.image and not captcha.busy then
    for i = 1, 8 do
      if captcha.keys[i] and in_rect(x, y, key_rect(i)) then
        click_emoji(i - 1)
        break
      end
    end
  end
  dirty = true
end

-- Callbacks ---------------------------------------------------------------

local app = {}

function app.on_start()
  load_config()
  load_settings()
  render()
end

function app.on_tick()
  if message and esp.time.millis() >= message_until then
    message = nil
    dirty = true
  end
  run_slice()
  if dirty or loading then render() end
end

function app.on_touch(x, y)
  touch = {x = x, y = y}
  if page == "board" then tap_board(x, y)
  elseif page == "thread" then tap_thread(x, y)
  elseif page == "captcha" then tap_captcha(x, y) end
end

function app.on_touch_end(x, y, held_ms)
  if page ~= "home" or not touch then return end
  local start = touch
  touch = nil
  local index = (start.y - 84) // 40 + 1
  local entry = start.y >= 84 and settings.boards[index]
  if entry and held_ms >= HOLD_MS then
    table.remove(settings.boards, index)
    save_settings()
    notify("Removed /" .. entry.id .. "/")
  elseif entry then
    load_board(entry.id)
  else
    tap_home(start.x, start.y)
  end
  dirty = true
end

function app.on_input(text)
  local purpose = prompt_for
  prompt_for = nil
  if purpose == "board" then
    if not text then return end
    local id = board_from_input(text)
    if id then load_board(id) else notify("Board names use a-z and 0-9") end
  elseif purpose == "reply" and thread then
    if not text then return end
    text = text:gsub("^%s+", ""):gsub("%s+$", "")
    if text == "" then notify("Empty reply") return end
    thread.draft = text
    start_captcha(text)
  end
end

function app.on_resume()
  dirty = true
end

function app.on_response(id, r)
  local p = pending[id]
  pending[id] = nil
  if loading and loading.id == id then loading = nil end
  dirty = true
  if not p then return end
  absorb_cookies(r.headers)
  local failed = (r.error and unreachable[r.error]) or challenged(r)
  if failed and p.opts.method == "GET" and p.tried < #MIRRORS then
    settings.mirror = settings.mirror % #MIRRORS + 1
    send(p.path, p.opts, p.handler, p.label, p.tried + 1)
    return
  end
  if challenged(r) then
    p.handler(nil, "2ch is asking for a browser check; try later")
    return
  end
  if r.ok and p.tried > 1 then save_settings() end
  if r.error == "Cancelled" then return end
  p.handler(r)
end

return app
