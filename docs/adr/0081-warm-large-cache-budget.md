# 0081 — Adaptive warm large-cache budget

Accepted 2026-11-24.

## Context

The per-thread large-block cache budget has been 64 MiB since ADR 0019.
ADR 0074/0078 made retention selective: only buckets inside the hot window
are cached, and buckets already proven to serve hits get a wider window.
The throughput profile raises the budget to 256 MiB and was the fastest
profile on g++ and sort in the full CI matrix, but it also disables page
purge, which hurt xz.

## Decision

A bucket that has actually served a cache hit (`fm_lcache_warm_mask`) may
retain twice the configured budget, capped at 128 MiB.  Cold buckets keep
the configured budget exactly.  The rule composes with the profiles:
footprint 8 -> 16 MiB, shortlived 0 stays 0, throughput 256 MiB is already
above the cap.  Page purge and the hot window are unchanged.

```c
budget = FM_CFG()->lcache_max_bytes;
if (warm_mask & (1ull << b))
    budget = min(budget * 2, 128 MiB);
```

## Evidence

`exp/lcache-budget`, 3 repetitions, CI runner:

| case | ratio |
|---|---:|
| `gxx_compile` | 0.971 |
| `sort_p4` | 0.973 |
| `sqlite_oltp` | 0.989 |
| `jq_large` | 0.991 |
| `python_data` | 0.993 |
| real-app geomean | **0.9861** |
| micro geomean (Ir) | 1.0000 |

Peak RSS was flat on every app in that run.  A runtime sweep at
`FM_LCACHE_MAX_BYTES=256 MiB` (no code change) also measured 0.9918 on the
same apps, so the win is not specific to the warm/cold split; the split
keeps the cold-teardown behaviour of ADR 0074 intact.

## Risks

* `fm_lcache_warm_mask` is sticky per thread, so a bucket warm in an
  earlier phase can retain up to 128 MiB later.  The 64-free hot window
  still bounds it.
* More resident retention can raise peak RSS on a workload whose phases
  are longer than the benchmark's; the footprint profile remains the
  eager-return option.
* Cache accounting uses `map_size`, which ADR 0080's reserve inflates.
