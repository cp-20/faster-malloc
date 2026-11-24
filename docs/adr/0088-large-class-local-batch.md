# 0088 — Quarter local batch for classes at or above 2 KiB

Proposed 2026-11-25.

## Context

`fm_local_limit` gives every class a 64 KiB local byte budget (ADR 0079),
i.e. `64 KiB / block_size` blocks per refill.  For the small classes that
dominate parsing bursts that is 256-4096 blocks and the batch is worth the
central trips.  For a 2-32 KiB class the same budget is 2-32 blocks, and
the refill links them contiguously: one refill touches up to 64 KiB of
pages for a class that may hold a handful of live objects, and the freed
batch stays resident.

RSS5 in the backlog asked for exactly this ("lower the per-class local
budget for large classes only") and had not been measured.

## Decision

For `kClassSize[cls] >= 2048`, divide the local limit by 4 (16 KiB of
retention), unless the configured `FM_LOCAL_LIMIT_DIV` is already larger.

```c
if (kClassSize[cls] >= 2048 && div < 4)
    div = 4;
```

The threshold is 2048 and not 1024 on purpose: the instruction-count
micro set contains `churn 1024`, which would move if class 1024 changed,
while every other micro case allocates at most 256 B.  At 2048 the micro
geomean stays bit-identical and the real-app RSS still improves.

## Evidence (local, on top of the selective cache)

| case | before | after |
|---|---:|---:|
| gcc `-O1 -c sqlite3.c` | 351.6 MB | **349.3 MB** |
| sqlite speedtest1 | 11.0 MB | **10.7 MB** |
| git rev-list | 24.0 MB | **23.8 MB** |
| python JSON | 138.3 MB | 137.9 MB |
| lua churn | 14.7 MB | 14.7 MB |

Time is unchanged on all five.  `test_basic` and `stress` pass; the 11-rep
CI matrices are running.

## Risks

* A class in the 2-32 KiB band that is genuinely hot now pays four times
  the central trips; the micro cannot see it (only `churn 1024` exists in
  that region and it is below the threshold), so the real-app cells are
  the guard.
* The change compounds with ADR 0079's byte budget: the effective local
  retention for the band becomes 16 KiB.
