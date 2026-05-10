local headers = {}
headers["Content-Type"] = "application/json"

local payloads = {}
for i = 1, 50 do
  local f = io.open("payloads/payload-" .. i .. ".json", "rb")
  payloads[i] = f:read("*all")
  f:close()
end

local counter = 0

request = function()
  counter = counter + 1
  local idx = ((counter - 1) % 50) + 1
  return wrk.format("POST", "/fraud-score", headers, payloads[idx])
end
