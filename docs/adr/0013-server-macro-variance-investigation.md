# 0013 — Server macro: apparent v2 loss to v1 was measurement noise

*Status: Closed — no allocator change (2026-11-12). Recorded
contemporaneously.*

## Hypothesis

The first macro baseline showed the `server` scenario at 196.9 ns/op
for v2 against 140.6 for v1 — the only macro where v2 lost. Candidate
causes: central-batch CAS traffic and frequent flush/refill cycling
when frees spread over ~130 classes and 25 % of them cross threads.

## Investigation and evidence

1. A second batch (after the ADR 0011 work) put glibc first (103.1)
   and v1 last (183.8) — a complete ranking inversion between batches.
   The scenario was not measuring allocator differences reliably.
2. A benchmark flaw was found and fixed: teardown (freeing the live
   pool and *spin-draining* the transfer queue) was inside the timed
   region, so with 4 worker threads on ~1.5 CPUs, early finishers
   burned the shared CPU spinning while the last workers were still
   being timed. The timed region now ends at each worker's main-loop
   end (max over workers).
3. After the fix, 4 threads, min-of-5: glibc ~159, v1 ~174, jit ~192,
   v2 ~199 ns/op — but run-to-run ranges within one allocator span
   ±25 % and overlap across allocators.
4. At 2 threads (less oversubscription), the picture stabilises:
   v1 47.4–49.3, v2 49.4–50.2, glibc 65.1–82.2. v1 and v2 are tied
   within a few percent; both are well ahead of glibc.

## Decision

No allocator change. The apparent deficit was an artefact of CPU
oversubscription and the teardown-timing flaw, not a v2 weakness this
host can demonstrate. Per ADR 0009, a claim this size needs the
exact metric (callgrind instruction counts), which is unavailable on
this VM (no valgrind). If the question is revisited, it should be on
a quieter host or with instruction counting, and the fixed timing
boundary is already in `bench/macro.c`.
