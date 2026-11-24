# 0087 — Selective large-cache retention

Proposed 2026-11-25.

## Context

ADR 0083 lowered the balanced large-cache budget from 64 MiB to 16 MiB.
That fixed most of node's RSS gap but cost it about 3 points of time,
because node reuses multi-MiB JSON buffers and a smaller budget sends
them back to the kernel.  The budget is a blunt instrument: it says
nothing about *which* multi-MiB blocks are worth keeping.

The cache already knows which buckets have served a hit
(`fm_lcache_warm_mask`, ADR 0081).  A bucket that hit is reused by
construction; a bucket that never hit is a free-and-forget arena -- the
canonical example is CPython's 1 MiB obmalloc arena, which is allocated,
filled, freed and never touched again.

## Decision

Replace the flat byte budget check with a per-bucket retention rule on
top of the 16 MiB base:

* a **warm** bucket (one that has served a cache hit) may retain up to
  4 MiB;
* a **cold** bucket keeps at most one block, and only if it is at most
  256 KiB;
* `fm_lbucket_bytes[]` tracks retained bytes per bucket so insertion,
  cache hits, the carve path and the trim drain all keep it exact.

```c
warm = fm_lcache_warm_mask & (1ull << b);
take = warm ? (bucket_bytes[b] + ms <= 4 MiB)
            : (bucket_bytes[b] == 0 && ms <= 256 KiB);
```

## Evidence (local, interleaved against the 16 MiB build)

| case | 16 MiB | selective |
|---|---:|---:|
| `py_pipeline` JSON half | 142.0 MB | **138.3 MB** |
| lua churn | 16.3 MB | **14.4-15.5 MB** |
| sqlite speedtest1 | 11.5 MB | **11.1 MB** |
| git rev-list | 24.0 MB | 24.0 MB |
| node churn | 404.9 MB | 404.1 MB |

Time is unchanged on every case; `test_basic`, `stress`, `rtest`,
`valgrind` and `mtrepeat` pass.  The 11-rep CI matrices are running.

## Risks

* The warm mask is sticky, so a bucket that hit once may retain up to
  4 MiB for the rest of the process; the global 16 MiB budget still
  bounds the total.
* Cold buckets are capped by count *and* size, so a cold 1 MiB arena is
  never cached; a workload that reuses a 1 MiB block without ever
  hitting would lose that reuse.
