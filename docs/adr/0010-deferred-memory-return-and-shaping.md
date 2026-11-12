# 0010 — Deferred: memory return to the OS and free-list shaping

*Status: Partially resolved by round 2 (2026-11-12). Provenance:
Reconstructed (2026-11-12, from README §6, "Where v2 is not the
best" and "What I would do next").*

*Round 2 outcome: item 1 (memory return) is delivered for the
explicit-trim path by ADR 0015 — spans are unmapped by
`malloc_trim`, not automatically; automatic release on the free
path was measured and rejected (ADR 0014). Item 2 (free-list
shaping) was implemented, measured, and rejected as the default
recycling path by ADR 0014; it remains open in the weaker form
recorded there (refill-side regrouping, or revisiting on hardware
with cache counters).*

## Context

Two known gaps were recorded when v2 was finished:

1. **Memory footprint.** v2's peak RSS on espresso is 9.0 MB against
   glibc's 3.4 MB. Spans are never returned to the OS and the
   per-thread caches are unbounded in aggregate. A production
   allocator needs a `malloc_trim` that actually releases spans.
2. **Locality on reuse.** glibc and mimalloc have fewer D1 misses on
   churn because they reuse the same physical page repeatedly;
   mimalloc's abandoned-page mechanism also keeps a freed block with
   the thread that freed it. v2 hands a block to whichever thread
   frees it, with no affinity for where it was allocated
   ("free-list shaping" in the literature).

## Decision

Defer both. They were judged less valuable than correctness and the
fast path for this project's goals, and free-list shaping in
particular changes the core list discipline (0005), so it should be
its own measured experiment, not a side change.

## Consequences

Any future work on RSS or reuse locality starts here. The macro
benchmarks added 2026-11-12 report peak RSS alongside time so a
regression in footprint is visible when these are picked up.
