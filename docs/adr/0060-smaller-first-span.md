# 0060 — Smaller first span per size class

Status: Accepted for static v2 (2026-11-13). JIT application rejected in ADR 0061.

## Hypothesis and attempt

The first use of a class already allocates lazily, but `fm_span_new` links a
whole 64 KiB span. A single allocation therefore writes link words throughout
that span. At paused startup, Python has 4,292 KiB of resident small-span pages
for 467 KiB of live small usable capacity; Node has 6,104 KiB for 1,056 KiB.
Those capacities are not RSS; the comparison identifies substantial spare
capacity, not an exact resident-fragmentation subtraction.

Try a 16 KiB **first** span per class, rounded up to fit at least one block plus
the header for larger classes. Later spans retain 64 KiB. A process-wide atomic
byte per class selects the first request even under concurrent refills; it is
only accessed on fresh-span allocation, never on malloc/free fast paths. An
allocation failure may consume the small-first opportunity, which affects only
footprint. No new runtime option or thread-local array is introduced.

Keep 64 KiB alignment and the existing pointer-to-header mask. Each span's
`nblocks` and `map_size` are immutable and describe its actual mapping from
creation. Census coverage and withheld-block reconstruction already use
`nblocks`. Change `mincore` to query the actual mapping length, avoiding ENOMEM
from querying its unmapped suffix. Empty pages beyond the shorter mapping have
zero total/start counts and cannot become purge candidates.

## Correctness scope

ADR 0035's census mutex, central ownership locks, batch-header lifetime rules,
atomic marks and thread-local result counters are untouched. The intersect and
start-count proof, withheld set, age gates, fork quiescence, and conservative
treatment of other threads' local lists are unchanged. Smaller geometry does
not authorize reclaiming any additional live or invisible block.

Only fm_static.c enables the compile-time policy; JIT retains the prior first
span without the extra atomic. The new white-box check covers all 129 classes: exact first-map geometry and
links, a fully written live first block across a partial-span census, whole-span
release including withheld blocks, then normal 64 KiB growth. It runs in release
and FM_DEBUG with one-page purge enabled. The complete release/debug round7
batteries and intensified debug stress are required, not just test_basic.

## Measurement and decision

Raw results, the noisy initial timing guard, CPU-time follow-up, acceptance
checks and limitations are in `bench/results/round12-startup/` and
`docs/round12-2026-11-13.md`. Seven rotated startup repetitions compare the exact
round-10 wrapper method and a target-only-preload control, with static/JIT and
installed jemalloc. Three synchronized snapshots independently verify the
component reduction; a diagnostic build measures live usable capacity and batch
metadata. No diagnostic timing is performance evidence.

The policy is adopted only for static v2. Its startup ranges are strictly
separated in both timing methods. Across all 16 timing pairs, static paired
wall-time median changes (95% bootstrap intervals) are Python -6.03%
(-14.49 to +3.30), Node +6.45% (-8.96 to +16.81), KV +2.90%
(-13.99 to +16.24). All include zero, and every individual guard range overlaps.
The nine-pair CPU guard also includes zero for every static workload. This is
no detected regression beyond this session's noise, not proof of narrow
performance equivalence or a speedup. The five-pair final scoped confirmation
also has overlapping wall/CPU ranges. Including all 21 static pairs gives
Python -6.36% (-13.99 to +3.30), Node -0.39% (-8.96 to +9.22),
KV +3.70% (-5.98 to +11.12); none resolves a slowdown. Raw first-pass median shifts remain
reported rather than hidden. JIT's pooled churn signal fails that same gate
and is separately rejected in ADR 0061.

This is the only policy family attempted in this section. Smaller per-thread
arrays, eager metadata deferral and a lazy-carving state machine were not
implemented: TLS/JIT metadata are small, and a fixed smaller first mapping can
reuse the existing census proof without mutable span geometry.
