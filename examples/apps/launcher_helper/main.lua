-- Hidden from Home ("show_in_home": false); the Launcher example opens it.
local opens = (tonumber(esp.storage.read("opens.txt") or "0") or 0) + 1
esp.storage.write("opens.txt", tostring(opens))

return {
  on_start = function()
    esp.ui.show({
      title = "Launcher Helper",
      {kind = "text", text = "This app has no Home tile."},
      {kind = "text", text = "Opened " .. opens .. " time(s)."},
      {kind = "text", text = "Close returns to Home."},
    })
  end,
}
