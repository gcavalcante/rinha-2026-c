local headers = {}
headers["Content-Type"] = "application/json"

local payloads = {}

for i = 1, 50 do
  local path = "payloads/payload-" .. i .. ".json"
  local f, err = io.open(path, "rb")

  if not f then
    error("payload nao encontrado: " .. path .. " erro: " .. tostring(err))
  end

  payloads[i] = f:read("*all")
  f:close()
end

local counter = 0

request = function()
  counter = counter + 1
  local idx = ((counter - 1) % 50) + 1
  return wrk.format("POST", "/fraud-score", headers, payloads[idx])
end
