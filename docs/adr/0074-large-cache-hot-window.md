# 0074 — Stop caching large blocks for teardown-shaped buckets

Status: Accepted (2026-11-16, commit cb178ee).

## Change

The large-block cache kept every freed large mapping up to its 64 MiB budget
regardless of reuse — a one-shot teardown (the large half of a
free-everything workload) retained all of it until malloc_trim; glibc munmaps
such blocks outright, which is why its after-free RSS was ~25 MB lower.
The cache is now per-bucket hot: `fm_ltick` counts this thread's large frees,
`fm_lbucket_seen[b]` is stamped by every allocation search probing bucket b,
and a free caches only while fewer than `FM_LCACHE_HOT_WINDOW` (8) large
frees have passed since that bucket was last allocated from. Cycling
workloads re-stamp on every allocation and never leave the window; teardowns
fall out after 8 frees and unmapping resumes. Buckets start seen-at-tick-0 so
the first free of a fresh bucket still caches (seeding a reuse cycle works;
test_basic's large-cache reuse check passes).

## Results

bench/macro trim: rss_after_free 151300–154264 → 125972–128936 KB (glibc
124672); rss_after_trim unchanged (~3020 KB). Ir/op unchanged on all six
measured modes; large-mode 1.3 → 1.6 Ir/op from the tick increment;
wall-clock parity on instr large/batch; mt/macros/jq/locality parity;
v2/jit test_basic+stress pass.

Rejected alternative: per-bucket caps or byte budgets sized by request size
(cannot distinguish cycle from teardown); hit-rate-based gating (needs the
cache to exist before a hit can be observed).
