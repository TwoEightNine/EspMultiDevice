local count = tonumber(esp.storage.read("count.txt") or "0") or 0

local function show()
  esp.ui.show({
    title = "Counter",
    {kind = "text", text = "Count: " .. count},
    {kind = "button", id = "add", text = "Add one"},
    {kind = "button", id = "reset", text = "Reset"},
  })
end

return {
  on_start = show,
  on_action = function(id)
    if id == "add" then count = count + 1 end
    if id == "reset" then count = 0 end
    esp.storage.write("count.txt", tostring(count))
    show()
  end,
}
