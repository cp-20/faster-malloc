# 0012 — calloc: skip the memset for memory known to be zero

*Status: Deferred after measurement (2026-11-12). Recorded
contemporaneously.*

## Hypothesis

`fm_calloc` memsets every small allocation, but blocks carved from a
fresh span sit on anonymous mmap pages that are already zero. A
per-span "virgin" flag (set when the span is cut, cleared on the first
free into the span) would let calloc skip the memset for those blocks.
A first baseline batch seemed to support this strongly: calloc 4 KiB
measured 9203 ns/op for v2 against 881 for glibc.

## Evidence against

The 9203 figure did not survive an interleaved re-run. Measured
allocator-against-allocator in the same rounds, calloc 1×4096 is
**~70 ns/op for v2 vs ~88 ns/op for glibc** — v2 already wins, and a
warm 4 KiB memset costs only ~64 ns on this host. The original batch
was inflated by a CPU-throttling episode on the shared VM (the same
batch's other numbers were unaffected because they ran earlier).

## Decision

Deferred. In the workload where the memset is actually paid (a
recycled block) it is cheap; the virgin-span win would only appear for
calloc of never-touched span memory that the caller also never reads
before freeing, which is rare in real programs. Revisit only if a
macro benchmark shows a calloc-heavy pattern losing to glibc. The
episode is recorded because it changed the plan: two of the three
"obvious" optimisation targets from the first baseline (calloc, and
half of pareto) were measurement artefacts or second-order; the large
path (ADR 0011) is the real one.
