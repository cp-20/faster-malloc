# 0015 — malloc_trim that actually returns spans: the central-list census

*Status: Accepted (2026-11-12). Recorded contemporaneously.*

## Context

Since round 1, `malloc_trim` only drained the large-block cache;
small spans were never returned to the OS at all, so a program that
peaked and then freed everything stayed at its peak RSS unless the
memory happened to be large blocks. ADR 0010 deferred "a
`malloc_trim` that actually releases spans"; this is that decision.

Scope honesty: trim only helps a program (or runtime) that calls
it, and does nothing for a peak measured while the working set is
live. The espresso gap in README §6 (v2 9.0 MB vs glibc 3.4 MB peak)
is a peak problem — concentration and retention policy, not trim —
and this ADR does not claim to close it. What trim closes is the
post-peak retention gap, which is the part a long-running process
that never calls trim would otherwise hold forever. Releasing
spans *automatically* on the free path was measured in ADR 0014
and rejected there: the mmap/fault cycle costs more than the
retention it saves, which is also why glibc only trims on request.

## Change

`malloc_trim` (both builds) now:

1. Drains the large-block cache (as before).
2. Runs a per-class census (`fm_trim_spans` in fm_core.h): flush
   the calling thread's local lists, atomically detach the class's
   whole central stack, and count free blocks per span. A span
   whose count equals its block count is *dead*: every block exists
   exactly once across all the allocator's lists, so if all of them
   are here, none are live and none are held anywhere else. Dead
   spans are unmapped; surviving blocks are re-batched and pushed
   back. Other threads' local lists are out of reach, which only
   ever under-counts — the safe direction.

Supporting change: batches now record their length and whether
they are span-pure (`fm_publish2`), where the publisher knows them
for free — currently the fresh-span rest at refill, which is the
common case after any allocation burst. The census accounts for
those wholesale and walks the rest (flush-published batches record
no count, since counting them would need the walk ADR 0014
rejected). An FM_DEBUG build asserts the census never over-counts
a span; it held across the full test battery.

`malloc_trim` returns 1 when anything was released, 0 otherwise
(glibc semantics; the old stub always returned 1).

## Evidence

New `trim` macro scenario: build a ~157 MB working set (120,000
mixed small blocks + 200 × 128 KiB), free it all, call
`malloc_trim(0)`, reading VmRSS at each stage.

| allocator | live | after free | after trim |
|---|---|---|---|
| glibc | 150.9 MB | 124.8 MB | **2.8 MB** |
| v2 before | 157.3 MB | 161.6 MB | 135.2 MB |
| v2 after | 157.4 MB | 161.7 MB | **7.0 MB** |
| jit after | 157.4 MB | 161.7 MB | **7.1 MB** |

The remaining ~4 MB over glibc decomposes into the process
baseline, spans pinned by a handful of live libc/stdio blocks, and
batch-pool pages (which are never returned — a known, bounded
retention of one 4 KiB page per 128 batch headers ever live at
once). A controlled single-class diagnostic (20,000 × 64 B) lands
at near-parity: v2 3,468 → 2,216 KB across trim, glibc 3,508 →
1,944 KB.

Cost on the hot path: none measurable. Same-minutes probe against
the pre-round-2 build: churn 8 B 6.76 vs 8.74, churn 64 B 7.44 vs
7.38, batch 8.78 vs 8.79, hold 11.24 vs 11.16 ns/op — the count
plumbing is two stores on already-cold paths.

Correctness: `test_basic` gained a span-release section —
span-filling allocations freed and trimmed in rounds, and a live
set held *across* a trim whose patterns are verified afterwards
(the census must never release a span with live blocks). Full
battery (test_basic, stress ×3, check, mt, macro) passes under v1,
v2, jit and an FM_DEBUG v2.

One measurement-discipline note, in the spirit of README §6's
harness bugs: the first diagnostic for this change appeared to
show the first `malloc_trim` releasing nothing and the second
everything. The census was fine; the diagnostic printed
`malloc_trim(0)` and `rss()` as arguments to the same `printf`,
and the compiler evaluated `rss()` first. The macro scenario
(which sequences the calls in separate statements) had the true
numbers all along.

## Decision

**Accepted.** `malloc_trim` is now a real release valve for small
spans as well as large blocks, at zero hot-path cost. Known
limits: it cannot see other threads' local lists (a span with
blocks parked there survives until those threads flush — the same
limitation glibc has with per-thread caches), and in the worst
case it walks every free block of a class, which is the right
price for an explicitly cold entry point.
