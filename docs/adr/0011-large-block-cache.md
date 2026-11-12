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
(power-of-two buckets); overflow beyond a per-thread byte cap is
munmap'd as today. Only plain large allocations (alignment 0) are
cached; aligned large allocations keep the munmap path.
`fm_large_alloc` first tries the bucket for the required mapping size
and reuses a block whose mapping is large enough; the span header's
`user_size` is updated on reuse. calloc learns whether the block came
from a fresh mapping (zero) or the cache (stale, must memset) via an
out-parameter — the one correctness trap in this design.

## Evidence

Round 1 — 2 blocks per bucket, 16 MiB cap:

- `large 65536`: **~9000 → 8.7 ns/op** (glibc 28). The reuse loop is
  exactly what the cache was built for.
- `pareto 50000×10`: **unchanged (~3000)**. Pareto allocates 50k mixed
  blocks, holds them all, frees them all, and repeats; ~10k blocks per
  round are on the large path. A 2-deep bucket can absorb none of the
  burst, so rounds re-mmap everything. A control run with the size
  distribution capped at 16 KiB measures 206 ns/op for v2, so the
  > 16 KiB tail is ~93 % of v2's pareto time (glibc ~1400 overall
  because it retains freed memory in its arena — its pareto peak RSS
  is 242 MB against v2's 87 MB).

Round 2 — keep the mechanism, change the retention policy: per-bucket
intrusive lists (through a new `lnext` field in the span header's
reserved space) and a 256 MiB per-thread cap on cached *virtual*
bytes. Untouched cached pages cost no RSS; this is the same retention
every production allocator does (glibc's mmap-threshold adaptation
and heap top, jemalloc's dirty-page decay), with an explicit bound.

Result: `large` stayed at ~9 ns/op, but **pareto still ~2900–3040**.
Per-round timing of a pareto replica showed why: the alloc phase
improved across rounds (4801 → ~1950 ns/op) but the *free* phase
stayed ~900 ns/op — once the cap fills, every remaining large free is
an munmap again, and ~7200 of ~10050 large frees per round overflow
it. Recomputing the distribution explains the scale: 11.8 % of pareto
allocations are a full 1 MiB, so one round holds ~6 GB of large
mappings. No modest cap retains that working set.

Round 3 — attack the uncached path's syscall count instead. v2's
large alloc did mmap + head-trim munmap + tail-trim munmap, and free
did a fourth syscall (munmap); glibc does one mmap and one munmap.
New `fm_map_large`: over-map by one span, place the header at the
64 KiB-aligned base inside the mapping, keep the head slack mapped
(it is never touched), record the *raw* mapping in
`map_base`/`map_size`, and release everything with a single munmap
at free. 4 syscalls per pair → 2.

Result: pareto **~3000 → ~1770**, pburst alloc steady-state ~750
ns/op (glibc ~830). `large 65536` temporarily regressed to ~6000–
8000 — see round 4.

Round 4 — **a correctness bug in round 3, caught by `bench/stress`
under the FM_DEBUG invariant checks.** With the slack included,
`map_size` overstated a cached block's usable length by up to 64 KiB,
so the reuse fit-check could hand out a block up to ~64 KiB too
small; the caller's tail writes then corrupted the neighbouring
mapping (in stress, a span header was overwritten with the edge
test's 0x5A fill — the debug walk found a class-123 list containing a
"block" inside the overwritten region). The v2 build segfaulted;
the JIT build passed by layout luck. Fix: bucket and fit-check by
*usable* length (`map_size` minus the head gap, `fm_large_usable`),
and search the next bucket up as well, since usable length can spill
one bucket above the request's. After the fix: stress passes under
FM_DEBUG, plus test_basic, bench/check and a targeted
dirty-block/large-calloc test, on all three builds. `large` back to
9.4 ns/op; pareto ~1750–1870.

Round 5 — retention cap, measured as a policy choice (pareto ns/op,
v2, RSS constant at ~87 MB throughout — retained pages are untouched):

| cap per thread | 256 MiB | 1 GiB | 2 GiB | 4 GiB | unbounded |
|---|---|---|---|---|---|
| pareto | ~1800 | ~1600 | 1521 | **~1000** | 353 |

glibc on the same workload: ~1400 ns/op at 242 MB peak RSS.

## Decision

**Accepted**, with the round-3 mapping change, the round-4 usable-
length fix, and a per-thread retention cap of **4 GiB** of virtual
address space (`FM_LCACHE_MAX_BYTES`). At 4 GiB v2 beats glibc on
pareto (~1000 vs ~1400) while its peak RSS stays at 87 MB against
glibc's 242 MB, because retained pages that are never touched cost
no memory. Unbounded retention is faster still (353) but was
rejected: an unbounded per-thread VM cache is a liability under
strict overcommit accounting, and 4 GiB already covers the large-
block working sets of the workloads measured. `malloc_trim` now
drains the cache, giving programs and the C runtime a release valve
(partially addressing ADR 0010). Known remaining cost: the cache is
per-thread and is not drained at thread exit (the mappings stay with
the process, like the existing span behaviour).

Final verification numbers are in `docs/opencode-2026-11-12.md`.
