-- SDK 3 voice recorder: the built-in Voice Recorder rebuilt on esp.mic and
-- esp.audio.play_file, plus an input level meter. The WAV is stored in app
-- data (/.system/appdata/recorder/voice_last.wav). Coordinates are relative to
-- the area below the app bar.
local black, grey, lightgrey, cyan = 0, 31727, 54938, 2047
local red, orange, green = 63488, 64928, 2016
local FILE = "voice_last.wav"

local buttons = {
  rec  = {x = 20,  y = 48, w = 84, h = 54},
  stop = {x = 118, y = 48, w = 84, h = 54},
  play = {x = 216, y = 48, w = 84, h = 54},
}

local status = "Ready"
local has_recording = false
local was_recording, was_playing = false, false
local mic_powered = true
local drawn = ""

local function in_rect(x, y, r)
  return x >= r.x and x < r.x + r.w and y >= r.y and y < r.y + r.h
end

local function refresh_recording()
  has_recording = esp.storage.size(FILE) ~= nil
end

local function render(mic, playback)
  local recording, playing = mic.recording, playback.playing
  local idle = not recording and not playing
  local sd_ok = esp.device.status().sd_mounted
  local rec_on = idle and sd_ok and mic.powered
  local stop_on = not idle
  local play_on = idle and sd_ok and has_recording

  local storage = "SD: " .. (sd_ok and "mounted" or "waiting") ..
                  "   Mic: " .. (mic.powered and "on" or "switch off")
  local details
  if recording then
    details = string.format("Recording %ds, %d KB",
                            mic.elapsed_ms // 1000, mic.bytes // 1024)
  elseif playing then
    details = string.format("Playing %ds", playback.elapsed_ms // 1000)
  else
    details = has_recording and "Saved: " .. FILE or "No recording yet"
  end
  local level = recording and (mic.level // 4) * 4 or 0

  local key = table.concat({storage, status, details, level,
                            rec_on and 1 or 0, stop_on and 1 or 0,
                            play_on and 1 or 0}, "|")
  if key == drawn then return end
  drawn = key

  esp.tft.begin_frame(black, "Voice Recorder")
  esp.tft.drawString(storage, 20, 10, lightgrey, black, 0, 0)
  local function button(r, label, color, enabled)
    esp.ui.button(r.x, r.y, r.w, r.h, label, enabled and color or grey,
                  enabled and black or lightgrey, black)
  end
  button(buttons.rec, "Rec", red, rec_on)
  button(buttons.stop, "Stop", orange, stop_on)
  button(buttons.play, "Play", green, play_on)
  esp.tft.drawString(status, 20, 122, cyan, black, 0, 0)
  esp.tft.drawString(details, 20, 148, lightgrey, black, 0, 0)
  esp.tft.drawString("Input level", 20, 190, grey, black, 0, 0)
  esp.ui.progress(20, 214, 280, 14, level, green, grey, black)
  esp.tft.present()
end

local function poll()
  local mic, playback = esp.mic.status(), esp.audio.playback()
  if was_recording and not mic.recording then
    -- Ended on its own: switch turned off, write failure, or time limit.
    status = mic.error or "Recording saved"
    refresh_recording()
  end
  if was_playing and not playback.playing then
    status = playback.error or (playback.finished and "Playback finished" or "Playback stopped")
  end
  if mic.powered ~= mic_powered then
    mic_powered = mic.powered
    if not mic.recording and not playback.playing then
      status = mic_powered and "Mic switch turned on" or "Mic switch turned off"
    end
  end
  was_recording, was_playing = mic.recording, playback.playing
  render(mic, playback)
end

return {
  on_start = function()
    refresh_recording()
    mic_powered = esp.mic.status().powered
    poll()
  end,
  on_touch = function(x, y)
    local mic, playback = esp.mic.status(), esp.audio.playback()
    local idle = not mic.recording and not playback.playing
    if in_rect(x, y, buttons.rec) and idle then
      local ok, err = esp.mic.start(FILE, 600)
      status = ok and "Recording..." or err
    elseif in_rect(x, y, buttons.stop) and mic.recording then
      local ok, result = esp.mic.stop()
      status = ok and "Recording saved" or result
      refresh_recording()
    elseif in_rect(x, y, buttons.stop) and playback.playing then
      esp.audio.stop_file()
      status = "Playback stopped"
    elseif in_rect(x, y, buttons.play) and idle and has_recording then
      local ok, err = esp.audio.play_file(FILE)
      status = ok and "Playing..." or err
    end
    poll()
  end,
  on_tick = poll,
}
