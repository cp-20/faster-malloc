# 0011 — Cache recently freed large blocks instead of munmap/mmap per operation

*Status: Proposed — experiment in progress (2026-11-12). Recorded
contemporaneously.*

## Hypothesis

v2's large path (> 32 KiB) does `fm_map_aligned` (an mmap, an
alignment trim, up to two munmaps) on every allocation and an munmap
on every free. On this VM the existing `large 65536` micro scenario
measures **~9000 ns/op for v2 vs 28 ns/op for glibc** (glibc serves
these from its arena), and the `pareto` scenario — whose tail reaches
512 KiB — measures ~3000 ns/op vs glibc's ~1400. If freed large blocks
are cached and reused, both should collapse towards the small-path
cost, and macro workloads with large buffers (server payloads,
document buffers, KV values) should improve with them.

## Change to try

Per-thread cache of freed large blocks, bucketed by mapping size
(power-of-two buckets), at most 2 blocks per bucket, with a per-thread
cap on total cached bytes (16 MiB); overflow is munmap'd as today.
Only plain large allocations (alignment 0) are cached; aligned large
allocations keep the munmap path. `fm_large_alloc` first tries the
bucket for the required mapping size and reuses a block whose mapping
is large enough; the span header's `user_size` is updated on reuse.

## Evidence

_(to be filled in: `large`, `pareto`, macro scenarios, RSS impact)_

## Decision

_(pending)_
