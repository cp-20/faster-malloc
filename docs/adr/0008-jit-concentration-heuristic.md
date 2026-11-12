# 0008 — JIT concentration heuristic: specialise only if hottest class ≥ 50 %

*Status: Accepted. Provenance: Reconstructed (2026-11-12, from README
§3.3–§3.4).*

## Context

A specialised dispatch pays a range test per hot class. On a diverse
size distribution those tests mispredict on nearly every allocation,
and the static arithmetic dispatch is faster. (mimalloc's 2.9 % branch
mispredict rate on the diverse `churnmix` workload, README §5.2, is
the same cost in another allocator.)

## Decision

Specialise only when the hottest class accounts for at least 50 % of
sampled traffic (`FM_JIT_MINSHARE`). Below that, install the generic
path and stop profiling, so the JIT costs nothing. Refinement: if
almost all of a class's traffic is one exact size, narrow its range
test to that size (`size − x ≤ 0`).

## Consequences

Without the step-aside the JIT was 15 % slower on diverse workloads;
with it, it ends up only 1–3 % behind the static path there, and ahead
on monomorphic workloads. An allocator can measure itself and choose
its strategy — that, more than the code generation, is the result this
project takes from the JIT.
