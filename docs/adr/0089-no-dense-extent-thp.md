# 0089 — Drop the dense-extent THP hint

Proposed 2026-11-25.

## Context

`fm_map_dense_span` opens 2 MiB extents for the small-class spans.  After
four warm extents it used to call `madvise(MADV_HUGEPAGE)` on the new
extent when the previous one had been consumed within 100 ms, so a
bursty allocator would back its spans with 2 MiB pages.

THP is `madvise`-mode on the CI runners, so the hint is exactly what
enables huge pages for this heap.  A huge page commits 2 MiB of RSS on
the first touch, so a process whose spans touch one page per 2 MiB (or
whose live set is only a fraction of an extent) pays up to 2 MiB of RSS
per extent for a handful of touched pages.  The legacy git fixture
issues four `madvise(MADV_HUGEPAGE)` calls in one run.

## Decision

Remove the hint.  Dense extents stay demand-paged at 4 KiB.  The extent
size, the span size, the class ladder and every fast path are unchanged.

```c
-        if (fm_dense_extent.warmup < 4) fm_dense_extent.warmup++;
-        else if (now - opened_ms < 100) madvise(base, extent_size, MADV_HUGEPAGE);
         fm_dense_extent.cursor = base;
```

## Evidence (local, interleaved)

| case | with hint | without |
|---|---:|---:|
| git rev-list | 23.9-24.0 MB / 0.04 s | **21.9-22.0 / 0.04** |
| lua churn | 16.5-16.6 MB / 0.31 s | **15.8-15.9 / 0.31** |
| gcc `-O1 -c sqlite3.c` | 370.0 MB / 8.4 s | 370.0 / 8.3 |
| sqlite speedtest1 | 11.5-11.7 MB / 2.20 s | 11.5-11.7 / 2.19 |
| python JSON | 142.0 MB / 0.34 s | 142.0 / 0.34 |

The instruction-count micro set cannot see this change at all: it is
callgrind Ir, and THP does not alter the instruction stream.  `test_basic`,
`stress`, `rtest`, `valgrind` and `mtrepeat` pass; the 11-rep matrices are
running.

## Risks

* A workload with genuinely dense span traffic loses huge-page TLB
  coverage.  The earlier THP work (v6c, v7) measured multi-MiB
  `batch 8 KiB` microbenchmarks speeding up 2.3x with THP, but those were
  the large-block slab, not the dense extent, and neither real-app matrix
  moved.  The 11-rep time cells are the guard here.
* Process memory that is dense enough to benefit keeps working with base
  pages; if a future workload regresses, the hint can return behind a
  config knob.
