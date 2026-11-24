# Experiment backlog

The recurring campaign picks the first unclaimed `TODO` items from this
file (see `GOAL.md` for the selection rules).  Update the status when a
result arrives; a measured "no" is a result.

`exp/combined-v2` is the accepted round-13 batch and was merged as PR
#1.  It is not a new campaign result; do not open another PR for it.

Status: `TODO` / `RUNNING` / `ACCEPTED` / `REJECTED` / `BLOCKED`.

## Accepted (in the 2026-11-24 batch)

| id | hypothesis | branch | result |
|---|---|---|---|
| L1 | spend the full 64 KiB per-class local byte budget | `exp/local-limits` | micro 0.9612, real 0.9955; ADR 0079 |
| R1 | one-request headroom for geometric large realloc growth | `exp/realloc-growth` | 7-rep real 0.9831 (sort 0.920); ADR 0080 |
| C1 | warm large-cache buckets get 2x budget, cap 128 MiB | `exp/lcache-budget` | real 0.9861, RSS flat; ADR 0081 |
| S1 | 4 MiB per-thread large slab | `exp/large-slab` | real 0.9901, RSS flat; ADR 0082 |

Combined branch `exp/combined-v2` at 11 reps: micro 0.9612, real
geomean 0.9845 / paired 0.9819, sort 0.138 -> 0.126 s.

## Current priority: RSS and the legacy dashboard

The campaign is not done until v2 is first in **both time and peak RSS**
on every app.  The new suite's RSS gaps (2026-11-24) are: g++ +3.5%,
jq +0.04%, sqlite +17.5%, Python +3.2%, git +15.8%, xz +0.5%, sort
+38.9% (all against glibc).  The dashboard's redis/sqlite/jq_legacy/lua/
python_legacy/node/gcc/git rows are stale and must be re-baselined with
`exp-legacy.yml` first.

| id | area | hypothesis | risk |
|---|---|---|---|
| RSS1 | all | move the balanced defaults toward the footprint profile (shorter age, keep=0, smaller lcache) and recover the time loss with the accepted fast paths; measure the Pareto point | med |
| RSS2 | sort | drain the per-thread large slab remainder and cold large-cache buckets when the process has been idle for N ms, not only on trim/exit | med |
| RSS3 | sort | cap the warm large-cache budget per size band (e.g. 8 MiB for 32-128 KiB) instead of one 128 MiB pool | low |
| RSS4 | git/sqlite | startup footprint: smaller first spans, fewer initial classes mapped, avoid the 2 MiB dense extent for processes that allocate < N KiB | med |
| RSS5 | g++ | release fully-free small spans during compilation pauses; lower the per-class local budget for large classes only | med |
| RSS6 | Python | drain per-thread caches at interpreter shutdown / thread exit earlier; verify peak not just final RSS | med |
| RSS7 | jq/xz | close the 0.04%/0.5% gaps with page-granular purge thresholds (6 pages) and no time loss | low |
| RSS8 | all | add per-app RSS decomposition (smaps/mincore) to the CI artifact so the next cycle targets the actual resident classes, not a guess | low |
| LGC1 | legacy | run `exp-legacy.yml` on current main and record time/RSS for redis, sqlite1, jq_legacy, lua, python_legacy, node, gcc, git_legacy | low |
| LGC2 | redis | reduce server RSS: spans/TLS retention under redis-benchmark; target glibc's 7.4 MiB | med |
| LGC3 | node | release V8 external/Buffer churn; target tcmalloc's time and glibc's RSS | med |
| LGC4 | sqlite1 | speedtest1 query path: time vs tcmalloc, RSS vs glibc | med |
| LGC5 | gcc | SQLite amalgamation compile: close the 8% time gap to mimalloc and the RSS gap | med |


## Rejected (do not repeat without a materially different mechanism)

| id | hypothesis | branch | result |
|---|---|---|---|
| X1 | enable span-grouped refill (`FM_LOCALITY_SORT`) by default | `exp/locality-on` | micro 1.2546, real 0.9930 |
| X2 | direct-mapped regroup table + enable by default | `exp/locality-fast` | micro 1.3193, real 0.9982 |
| X3 | raise page-purge threshold 4 -> 8 | `exp/purge-tune` | real 1.0019 |
| X4 | rewrite 33-64 B calloc zeroing | `exp/calloc-zero` | real 1.0126, sort +8% |
| X5 | require a THP fast-consumption streak | `exp/thp-policy` | real 0.9977, within noise |
| X6 | first small span from static `.bss` | `exp/startup` | real 0.9875 alone, but 0.9876 vs 0.9845 on top of combined-v2 |
| X7 | per-thread 8-span dense extent cache | `exp/dense-thread` | correctness: `micro cross` rc=1 |
| X8 | widen cached-mapping carve to 16/32 MiB | `exp/large-reuse` | real 0.9917, marginal |
| X9 | avoid 2 MiB rounding on realloc reserve | `exp/realloc-tune` | real 0.9866, worse than base 0.9831 |
| X10 | cap 16/32 B local limits at 1024 blocks | `exp/local-tune` | micro 0.9626, real 0.9882; worse than base |
| X11 | disable page purge (`FM_PAGE_PURGE=0`) | sweep | real 1.0060, xz +8.6% |
| X12 | disable automatic census (`FM_RELEASE_MIN_DEPTH=65536`) | sweep | real 0.9943, not enough |
| X13 | 256 MiB flat large-cache budget | sweep | real 0.9918 at 3 reps; warm-split adopted instead |
| X14 | halve local limits (`FM_LOCAL_LIMIT_DIV=2`) | sweep | real 1.0081, gxx +4.8% |

## Open hypotheses

| id | area | hypothesis | risk |
|---|---|---|---|
| G1 | g++ | widen the hot window from 8 to 16 for 64 KiB-1 MiB buckets only (cc1plus obstack cycles ~20 chunks) | low |
| G2 | g++ | give 512 B-4 KiB classes a 128 KiB local byte budget (the 64 KiB rule leaves them at the old cap) | low |
| G3 | g++ | reuse the 64 KiB obstack chunk mapping across functions via a dedicated bucket class | med |
| S2 | sort | 8 MiB per-thread large slab (follow-up to S1) | low |
| S3 | sort | `MADV_HUGEPAGE` on the per-thread large slab, only after the second refill | med |
| P1 | python | reduce thread-exit / TLS destructor work (`fm_thread_exit_flush` walks all classes) | med |
| T1 | git/sqlite | skip the TLS text-patch scan when the process has made few allocations, or make it lazy for `free` only | med |
| T2 | git/sqlite | tune `FM_SMALL_FIRST_SPAN` cap (8/32 KiB) for startup classes | low |
| J1 | jq | tighten the warm large-cache window for 32-128 KiB buckets (jq's DOM phase) | low |
| J2 | jq | reserve across medium classes for realloc growth below `SMALL_MAX` | med |
| XZ1 | xz | page purge at 6 pages instead of 4/8 (between the two measured points) | low |
| XZ2 | xz | `MADV_FREE` instead of `MADV_DONTNEED` for cached large payloads | med |
| M1 | multi-thread | fix the `micro cross` failure in `exp/dense-thread` (cached span must not be unmap-able while in service) | med |
| M2 | multi-thread | per-thread batch pool page (one 4 KiB page of headers per thread, no global lock on first use) | low |
| A1 | all | 128 KiB spans (regenerate `size_classes2.h`; free mask, dense extent, census, trim all follow) | high |
| A2 | all | 8-byte size classes below 128 B (regenerate ladder; `fm_class_of` arithmetic changes) | high |
| A3 | all | runtime profile auto-detection: choose throughput-like release/lcache policy from the observed size mix | high |
| A4 | all | use `calloc`'s zeroed flag for fresh small spans (ADR 0012 deferred) | med |
| A5 | all | shave the malloc/free fast path by one load/branch (micro only; CI Ir decides) | low |

## How to claim

The campaign manager assigns the next IDs.  An implementation agent
should use the branch name in the table, start from current `main`, and
follow `docs/exp/README.md`.  Do not reuse a branch name that already
exists; check `git ls-remote origin 'refs/heads/exp/*'`.
