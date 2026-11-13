-- churn2.lua -- instrumented fork of churn.lua for the round-6
-- lua investigation (ADR 0031).  Identical allocation behaviour;
-- at each phase boundary it prints Lua's own live-byte accounting
-- (collectgarbage "count") next to the process RSS read from
-- /proc/self/status, so live data and allocator retention can be
-- told apart phase by phase.  churn.lua itself is untouched.

local function rss_kb()
    local f = io.open("/proc/self/status", "r")
    if not f then return -1 end
    for line in f:lines() do
        local v = line:match("^VmRSS:%s*(%d+)")
        if v then f:close() return tonumber(v) end
    end
    f:close()
    return -1
end

local function snap(tag)
    local live_kb = collectgarbage("count")
    print(string.format("snap %-10s lua_live_kb=%8.0f rss_kb=%d", tag, live_kb, rss_kb()))
end

math.randomseed(12345)

local acc = 0
snap("start")

-- 1. string growth: build a ~2 MB string by concatenation, repeatedly.
for round = 1, 30 do
    local parts = {}
    for i = 1, 2000 do
        parts[i] = string.format("key-%d-value-%d;", i, (i * 2654435761) % 100000)
    end
    local s = table.concat(parts)
    acc = acc + #s
    if round == 1 then snap("p1-round1") end
end
snap("p1-done")

-- 2. table churn with a rolling window of 4096 live tables.
local window = {}
for i = 1, 600000 do
    local t = { id = i, name = "node" .. (i % 1000), v = (i * 37) % 991 }
    t.payload = { i, i + 1, i + 2, i + 3 }
    window[i % 4096] = t
    if i % 65536 == 0 then
        local c = 0
        for _, w in pairs(window) do c = c + w.v end
        acc = acc + c
    end
end
snap("p2-done")

-- 3. sort a large array of strings (comparison churn + temp strings).
local arr = {}
for i = 1, 100000 do arr[i] = string.format("%08d", (i * 48271) % 2147483647) end
snap("p3-built")
table.sort(arr)
acc = acc + #arr[50000]
snap("p3-sorted")

collectgarbage("collect")
snap("post-gc")
print("lua-churn checksum", acc)
