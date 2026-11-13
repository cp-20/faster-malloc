# bench.rb -- Ruby workload for the app matrix: a different GC /
# allocator interaction from Python and Node.  Deterministic
# (seeded) mix of:
#   1. a retained object graph (hashes, arrays, strings) -- the
#      shape of a web app's model/cache layer,
#   2. heavy transient churn (per-request objects), partially
#      retained in a ring buffer,
#   3. JSON document generate/parse cycles.
# Prints wall time, end RSS and a checksum the harness compares
# across allocators (identical work => identical checksum).
require "json"

srand(20261113)
t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
checksum = 0

# 1. retained graph: 120k nodes, each a small hash with string keys
graph = Array.new(120_000) do |i|
  { "id" => i, "name" => "node-#{i % 977}", "tags" => ["t#{i % 31}", "u#{i % 17}"],
    "score" => (i * 2654435761) % 1000 }
end
checksum += graph[54_321]["score"] + graph[999]["name"].bytes.sum

# 2. churn: 900k transient objects; every 64th retained in a ring
ring = Array.new(4096)
900_000.times do |i|
  o = { "k#{i % 53}" => "v" * (8 + (i % 89)), "n" => i,
        "a" => [i, i * 2, "s#{i % 41}"] }
  ring[i % 4096] = o if (i % 64).zero?
  checksum += o["n"] % 7 if (i % 131_071).zero?
end
checksum += ring.compact.sum { |o| o["a"][1] % 13 }

# 3. JSON: build a ~400 KB document and parse it back, 25 times
doc = { "items" => Array.new(4_000) { |i|
  { "id" => i, "title" => "item #{i} #{"x" * (i % 60)}", "price" => i % 5000,
    "ok" => i.even? } }, "meta" => { "count" => 4_000 } }
25.times do
  s = JSON.generate(doc)
  back = JSON.parse(s)
  checksum += back["items"][1_234]["price"]
end

el = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
rss = File.read("/proc/self/status")[/VmRSS:\s+(\d+)/, 1].to_i
puts({ secs: el.round(3), rss_kb: rss, checksum: checksum }.to_json)
