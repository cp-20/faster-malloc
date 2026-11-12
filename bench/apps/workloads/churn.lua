-- churn.lua -- allocation-heavy Lua workload.
--
-- Lua routes every allocation through one allocator function backed by
-- realloc, so this exercises the interposed allocator directly:
--   * string growth by repeated concatenation (realloc growth path)
--   * table churn: many short-lived tables plus a rolling live window
--   * string.format / table.concat over the live set
-- Deterministic (fixed seed).  Prints a checksum so an allocator that
-- corrupts memory changes the output, not just the timing.

math.randomseed(12345)

local acc = 0

-- 1. string growth: build a ~2 MB string by concatenation, repeatedly.
for round = 1, 30 do
    local parts = {}
    for i = 1, 2000 do
        parts[i] = string.format("key-%d-value-%d;", i, (i * 2654435761) % 100000)
    end
    local s = table.concat(parts)
    acc = acc + #s
end

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

-- 3. sort a large array of strings (comparison churn + temp strings).
local arr = {}
for i = 1, 100000 do arr[i] = string.format("%08d", (i * 48271) % 2147483647) end
table.sort(arr)
acc = acc + #arr[50000]

collectgarbage("collect")
print("lua-churn checksum", acc)
