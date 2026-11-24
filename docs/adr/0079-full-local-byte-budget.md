# 0079 — Spend the full per-class local byte budget

Accepted 2026-11-24.

## Context

`kClassLimit[cls]` caps every class at 256 blocks.  The generator already
intended a 64 KiB byte budget per class, but for small classes the block
cap binds first: a 16 B class retains 4 KiB, a 64 B class 16 KiB.  The
real-app workloads in the CI matrix (jq, Python, sqlite, g++) free in
per-class bursts, so the cap forces extra flushes and central-list round
trips on exactly the classes that matter most.

## Decision

`fm_local_limit()` takes the larger of the generated block cap and the
full 64 KiB byte budget:

```c
limit = max(kClassLimit[cls], 64 KiB / kClassSize[cls]);
```

The existing `FM_LOCAL_LIMIT_DIV` override is applied afterwards, so the
footprint profile still divides the result.  Classes at or above 256 B
are unchanged.  Per-thread retained bytes stay bounded at 64 KiB per
active class (roughly 0.5 MiB extra in the worst case).

## Evidence

`exp/local-limits`, 7 repetitions, CI runner (callgrind counts are exact;
wall clock is interleaved against the merge-base in the same job):

| metric | before | after | ratio |
|---|---:|---:|---:|
| micro geomean (7 workloads) | — | — | **0.9612** |
| `retain 80k` Ir | 4,574,265 | 4,100,003 | 0.8963 |
| `cross 80k` Ir | 4,613,276 | 4,197,452 | 0.9099 |
| `churn 64 B` Ir | 4,594,445 | 4,488,075 | 0.9768 |
| `churn 1 KiB` Ir | 5,017,487 | 5,030,222 | 1.0025 |
| real-app geomean | — | — | 0.9955 |

All six real apps were within ±1% and peak RSS was flat.  The one micro
regression is the 1 KiB churn workload (+0.25%), where the class is
already above the new budget's useful range.

## Risks

* Small-class flush batches can be larger (up to 4096 blocks for 16 B),
  so a flush walk has a longer tail; the census/regroup prompt stride is
  correspondingly smaller.
* More blocks are retained per thread in a cold process.  Measured RSS on
  the real-app suite did not move.
* `bench/micro cross` hoards blocks locally for longer, which was the
  main regression risk; the exact Ir counts improved there instead.
