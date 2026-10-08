-- SDK 2 interval workout. Coordinates are relative to the area below the app bar.
local black, white, grey, blue, cyan = 0, 65535, 31727, 31, 2047
local work_seconds, rest_seconds = 25, 10
local volume = esp.audio.volume()
local phase, stage, cycle = "setup", "init", 1
local paused = false
local stage_started, stage_saved, total_started, total_saved = 0, 0, 0, 0
local total_active = false
local init_beeps, work_beeps, rest_half, rest_warnings = 0, 0, false, 0
local drawn = ""

local function elapsed(now, started)
  return (now - started) % 4294967296
end

local function stage_elapsed(now)
  return stage_saved + (paused and 0 or elapsed(now, stage_started))
end

local function total_elapsed(now)
  if not total_active then return 0 end
  return total_saved + (paused and 0 or elapsed(now, total_started))
end

local function cue(frequency, duration)
  esp.audio.play_tone(frequency, duration)
end

local function text(value, x, y, size, color)
  esp.tft.drawString(tostring(value), x, y, color or white, black, size or 1, 4)
end

local function button(x, y, width, height, label, fill)
  esp.ui.button(x, y, width, height, label, fill, white, black)
end

local function render_setup()
  esp.tft.begin_frame(black, "Workout")
  local rows = {
    {"workout", work_seconds, 24},
    {"rest", rest_seconds, 88},
    {"volume", volume, 152},
  }
  for _, row in ipairs(rows) do
    text(row[1], 160, row[3], 0, grey)
    button(18, row[3] + 12, 72, 42, "-", grey)
    text(row[2], 160, row[3] + 33, 2)
    button(230, row[3] + 12, 72, 42, "+", grey)
  end
  text("Work and rest: 5-600 seconds", 160, 249, 0, grey)
  button(16, 368, 288, 48, "Start", blue)
  esp.tft.present()
  drawn = "setup"
end

local function clock(seconds)
  if seconds >= 3600 then
    return string.format("%d:%02d:%02d", math.floor(seconds / 3600),
                         math.floor(seconds / 60) % 60, seconds % 60)
  end
  return string.format("%02d:%02d", math.floor(seconds / 60), seconds % 60)
end

local function render_workout(now)
  local spent = stage_elapsed(now)
  local duration = stage == "init" and 5000 or
                   (stage == "work" and work_seconds or rest_seconds) * 1000
  local shown = stage == "init" and math.max(0, math.ceil((duration - spent) / 1000))
                              or math.floor(spent / 1000)
  local total = math.floor(total_elapsed(now) / 1000)
  local progress = math.min(100, math.floor(spent * 100 / duration))
  local key = table.concat({stage, cycle, shown, total, paused and 1 or 0}, ":")
  if key == drawn then return end
  esp.tft.begin_frame(black, "Workout")
  text("Total " .. clock(total), 160, 22, 0, grey)
  text("Cycle " .. cycle, 160, 50, 1, grey)
  text(stage == "init" and "Init" or (stage == "work" and "Workout" or "Rest"),
       160, 81, 1)
  text(shown, 160, 140, 2)
  esp.ui.progress(16, 191, 288, 14, progress, cyan, grey, black)
  button(16, 312, 288, 42, "Stop", grey)
  button(16, 368, 288, 48, paused and "Continue" or "Pause",
         paused and blue or grey)
  esp.tft.present()
  drawn = key
end

local function reset_cues()
  init_beeps, work_beeps, rest_half, rest_warnings = 0, 0, false, 0
end

local function begin_stage(next_stage, started)
  stage, stage_started, stage_saved = next_stage, started, 0
  reset_cues()
  if next_stage ~= "init" then cue(1000, 500) end
end

local function start(now)
  esp.audio.stop_tones()
  phase, cycle, paused, total_active = "running", 1, false, false
  total_started, total_saved = 0, 0
  begin_stage("init", now)
  drawn = ""
  render_workout(now)
end

local function duration_ms()
  return (stage == "init" and 5 or
          (stage == "work" and work_seconds or rest_seconds)) * 1000
end

local function advance(now)
  while stage_elapsed(now) >= duration_ms() do
    local next_started = (now - (stage_elapsed(now) - duration_ms())) % 4294967296
    if stage == "init" then
      total_active, total_saved, total_started = true, 0, next_started
      begin_stage("work", next_started)
    elseif stage == "work" then
      begin_stage("rest", next_started)
    else
      cycle = cycle + 1
      begin_stage("work", next_started)
    end
  end
end

local function update_cues(now)
  local spent, duration = stage_elapsed(now), duration_ms()
  if stage == "init" then
    local seconds = math.min(4, math.floor(spent / 1000))
    while init_beeps < seconds do
      init_beeps = init_beeps + 1
      cue(500, 125)
    end
  elseif stage == "work" then
    if work_beeps < 1 and spent >= duration / 3 then
      work_beeps = 1
      cue(1500, 125)
    end
    if work_beeps < 2 and spent >= duration * 2 / 3 then
      work_beeps = 2
      cue(1500, 125)
    end
  else
    if not rest_half and spent >= duration / 2 then
      rest_half = true
      cue(500, 125)
    end
    for i, warning in ipairs({3, 2, 1}) do
      local bit = 2 ^ (i - 1)
      if math.floor(rest_warnings / bit) % 2 == 0 and
         spent >= duration - warning * 1000 then
        rest_warnings = rest_warnings + bit
        cue(500, 125)
      end
    end
  end
end

local function in_rect(x, y, left, top, width, height)
  return x >= left and x < left + width and y >= top and y < top + height
end

local function adjust(value, change)
  return math.max(5, math.min(600, value + change * 5))
end

local function touch_setup(x, y)
  if in_rect(x, y, 16, 368, 288, 48) then
    start(esp.time.millis())
    return
  end
  local rows = {24, 88, 152}
  for index, top in ipairs(rows) do
    local delta = 0
    if in_rect(x, y, 18, top + 12, 72, 42) then delta = -1 end
    if in_rect(x, y, 230, top + 12, 72, 42) then delta = 1 end
    if delta ~= 0 then
      if index == 1 then work_seconds = adjust(work_seconds, delta)
      elseif index == 2 then rest_seconds = adjust(rest_seconds, delta)
      else
        local next_volume = math.max(0, math.min(10, volume + delta))
        if esp.audio.set_volume(next_volume) then volume = next_volume end
      end
      render_setup()
      return
    end
  end
end

return {
  on_start = render_setup,
  on_touch = function(x, y)
    if phase == "setup" then
      touch_setup(x, y)
    elseif in_rect(x, y, 16, 312, 288, 42) then
      esp.audio.stop_tones()
      phase, paused = "setup", false
      render_setup()
    elseif in_rect(x, y, 16, 368, 288, 48) then
      local now = esp.time.millis()
      if paused then
        stage_started, total_started, paused = now, now, false
      else
        stage_saved = stage_elapsed(now)
        total_saved = total_elapsed(now)
        paused = true
        esp.audio.stop_tones()
      end
      drawn = ""
      render_workout(now)
    end
  end,
  on_tick = function(now)
    if phase ~= "running" then return end
    if not paused then
      advance(now)
      update_cues(now)
    end
    render_workout(now)
  end,
}
