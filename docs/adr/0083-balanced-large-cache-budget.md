# 0083 — Smaller balanced large-cache budget

Accepted 2026-11-25.

## Context

ADR 0019 lowered the per-thread large-block cache from 4 GiB to 64 MiB and
ADR 0081 lets a bucket that has served a hit keep up to 128 MiB.  The
11-rep legacy matrix on main shows that the remaining RSS gaps are
dominated by Node: 527 MiB against glibc's 448 MiB (+17.5%), the largest
absolute gap in the set (+83 MiB).

RSS wave 1 concluded that the large-cache byte cap was not a lever, but
that work measured jq, sort, g++, Python and xz -- applications whose
large buffers are either reused (jq, sort) or small (g++, Python).  Node's
workload is the opposite: `JSON.parse`/`JSON.stringify` of a large payload
plus `Buffer` churn allocates many multi-MiB buffers, frees them, and
never touches them again, so the cache keeps them resident for the rest of
the process.

Local reproduction with `node bench/apps/workloads/churn.js big.json`:
`FM_LCACHE_MAX_BYTES=0` drops max RSS from 431 MB to 405 MB, while local
limits, page purge, release cadence and locality sort change nothing.

The runtime sweep on the legacy apps (3 reps, v2 column, glibc in
parentheses) is monotone:

| budget | node | python | gcc | redis |
|---|---:|---:|---:|---:|
| 64 MiB (main) | 527 | 458 | 373 | 6.4 |
| 32 MiB | 476 | 453 | 374 | 6.5 |
| 16 MiB | 468 | 457 | 374 | 6.3 |
| 8 MiB | 463 | 448 | 372 | 6.4 |

## Decision

Set the balanced `FM_LCACHE_MAX_BYTES` to 16 MiB and the warm cap to
32 MiB.  The throughput profile still raises the budget to 256 MiB;
shortlived (0) and footprint (8 MiB, doubling to 16 on a warm bucket) are
unchanged.

```c
#define FM_LCACHE_MAX_BYTES (16ull << 20)
#define FM_LCACHE_WARM_BUDGET_CAP (32ull << 20)
```

## Evidence

`exp/lcache16b`, 11 repetitions, CI runner:

| case | paired ratio | RSS ratio |
|---|---:|---:|
| `jq_large` | 0.9947 | 1.0000 |
| `python_data` | 0.9972 | 0.9999 |
| `sort_p4` | 0.9959 | 1.0030 |
| `gxx_compile` | 1.0000 | 1.0000 |
| `git_revlist` | 1.0000 | 1.0027 |
| `sqlite_oltp` | 0.9952 | 1.0012 |
| micro geomean (Ir) | **1.0000** | |
| real paired geomean | **0.9972** | |
| real paired worst | **1.0000** | |

The matrix is unchanged because none of those apps live in the multi-MiB
buffer band that Node churns; the win is concentrated where the retention
actually was, and time is neutral to slightly better.

## Risks

* A process that legitimately reuses a multi-MiB working set now pays a
  remap and first-touch cost.  sort's merge buffers show a mild local
  trend (0.12 s at 64 MiB versus 0.13-0.14 s at 16 MiB), which is why the
  budget was not lowered further; the 11-rep CI sort cell is unaffected.
* The warm doubling is sticky per bucket, so a phase that warms a bucket
  can still retain twice the configured budget.
