wrk.method = "POST"
wrk.headers["Content-Type"] = "application/json"

local file = io.open("sample-request.json", "rb")
wrk.body = file:read("*all")
file:close()
