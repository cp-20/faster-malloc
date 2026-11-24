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

## Autonomous queue (new worker tasks)

These are deliberately smaller and self-contained so the local
`opencode run` worker can pick one every 30 minutes.  Branch names are
`exp/auto-<id>`.

| id | area | hypothesis | risk |
|---|---|---|---|
| N1 | Python | release fully-free small spans during a run without a full census: track a per-class dead-span hint and unmap one span when its central count equals its block count | med |
| N2 | git/sqlite | reduce startup mappings: keep the dense extent but do not open a second 64 KiB span for a class until its first span is exhausted; measure the mapping count | med |
| N3 | node | huge-buffer purge for V8 external/Buffer churn: cold-purge >16 MiB blocks (base `exp/rss-cold-huge`) and measure the legacy node row | med |
| N4 | gcc/redis | re-run the legacy matrix with the libc-built Redis and the cold-purge candidate; report time/RSS for redis/gcc/node | low |
| N5 | micro | shave one load/branch from malloc/free without changing the slow paths; accept only an exact Ir win | low |
| N6 | xz | add per-app smaps/mincore instrumentation to the CI artifact so the xz 0.68% RSS gap is attributed, not guessed | low |
| N7 | all | trim on memory pressure: read `/proc/self/statm` on the large-free slow path and purge cold caches when RSS exceeds a high-water mark | high |
| N8 | all | 128 KiB spans (regenerate `size_classes2.h`; free mask/dense extent/census/trim all follow) | high |
| N9 | redis | reduce Redis's startup RSS: the libc-built redis now shows v2 6.4 MiB vs glibc 5.4; cut first-span/TLS mappings or the idle extent | med |
| N10 | node | node RSS 531 vs glibc 450: purge 8-16 MiB V8 external buffers (separate threshold from the 16 MiB default) | med |
| N11 | g++ | make the large slab adaptive: 2 MiB for single-thread/short processes, 4 MiB for multithread; target g++ RSS without sort time loss | med |
| N12 | git/sqlite | startup mapping diet: map the dense extent lazily and reuse one 64 KiB bootstrap span; target 13.2 -> 11.4 MiB | med |
| N13 | python | release fully-free spans at a higher cadence (lower release interval / keep=0) only when RSS is above a high-water mark | high |
| N14 | all | make the cold-purge threshold adaptive: 16 MiB default, 8 MiB when the process is above a mapped-bytes high-water | med |
| N15 | micro | shave one load/branch from malloc/free (fold the class and head load); accept only an exact Ir win | low |
| N16 | cross | reduce cross-thread free cost: avoid the central lock on the second free of a batch | med |
| N17 | all | 8-byte size classes below 128 B (regenerate the ladder; arithmetic dispatch changes) | high |
| N18 | all | 128 KiB spans (regenerate `size_classes2.h`; free mask/dense extent/census follow) | high |
| N19 | all | use MADV_FREE for cold cached large blocks instead of MADV_DONTNEED; measure peak RSS and time | med |
| N20 | all | cap realloc headroom for buffers that are freed without reuse (track a reuse flag) | med |
| N21 | python/g++ | purge pages of partially-free spans more aggressively, but only when an RSS high-water mark is exceeded | med |
| N22 | redis | reduce redis server RSS: first-span cap and TLS array size for single-thread servers | med |
| N23 | all | `FM_RELEASE_KEEP` per class: keep 0 for large classes, 2 for small | low |
| N24 | all | page-purge threshold adaptive by class: 2 pages for small classes, 4 for large | low |
| N25 | g++ | give classes >= 2 KiB a 128 KiB local byte budget (the 64 KiB rule leaves them at the old cap) | low |
| N26 | g++/jq | hot-window 16 for 64 KiB-1 MiB warm buckets only | low |
| N27 | sort | cold-purge threshold 8 MiB when RSS is above a 128 MiB high-water, 16 MiB otherwise | med |
| N28 | all | `MADV_FREE` for cached large payloads instead of `MADV_DONTNEED` | med |
| N29 | sort/xz | `MADV_HUGEPAGE` only for the per-thread large slab after the second refill | med |
| N30 | startup | 32 KiB first spans instead of 16 KiB (the 4 KiB attempt was worse) | low |
| N31 | startup | 8 KiB first spans for classes that only ever hold one block | low |
| N32 | startup | map the dense extent lazily: first span from `fm_map_aligned`, then switch | med |
| N33 | all | reduce TLS footprint: pack `head/tail/cnt` into one array of 16-byte cells | high |
| N34 | multi-thread | per-thread batch-header page, no global lock on first batch | low |
| N35 | all | flush only when the local list reaches 3/4 of the limit, not at the limit | low |
| N36 | multi-thread | central-list lock stripes for the 8 busiest classes | high |
| N37 | churn | FIFO free-list hand-out for large classes, LIFO for small | med |
| N38 | jq | realloc headroom 1.5x for buffers >= 1 MiB, 2x below | low |
| N39 | all | lower the large-cache budget to 32 MiB only for >8 MiB buckets | low |
| N40 | idle | enable the idle-release timer by default only after a 1 GiB mapped-bytes high-water | high |
| N41 | python | unmap fully-free spans at interpreter finalization (register an atexit handler) | med |
| N42 | node | drain the thread-exit cache earlier for worker threads (after 1 ms idle) | med |
| N43 | redis | avoid the first dense extent for single-threaded servers: first span from `mmap` | med |
| N44 | gcc | reuse 64 KiB obstack chunks from a dedicated bucket across functions | med |
| N45 | all | cold-purge 8 MiB blocks only when the large-cache budget is above 32 MiB | low |
| N46 | all | combine warm-huge + G3 + N23 + N12 on one branch; measure interactions | med |
| N47 | python | N1 span release gated on the class not being reused recently; remove the jq +1.8% regression | med |
| N48 | node | N10 8 MiB cold purge gated on an RSS high-water (N14) but only for >16 MiB blocks | med |
| N49 | all | N23 dead large-span threshold 2 KiB -> 4 KiB | low |
| N50 | all | N24 2-page purge only for classes < 256 B, 4 pages above | low |
| N51 | jq/sqlite | G3 window 16 keyed on size band instead of the hard-coded bucket 17 | low |
| N52 | g++/jq | N26 medium window 16 combined with warm-huge | low |
| N53 | sort/xz | N29 THP slab gate after the second refill; confirm with 11 reps | med |
| N54 | sort/xz | N39 huge-bucket 32 MiB cap combined with warm-huge | low |
| N55 | micro | N5 prefetch removal; check the rand/cross Ir and real apps | low |
| N56 | all | N8 128 KiB spans: fix the sort +1.6% and python +2% outliers | high |
| N57 | git/sqlite | N2 startup mapping + N12 lazy dense extent | med |
| N58 | git/sqlite | N31 one-block first span + N2 | med |
| N59 | redis | N43 exact first span + N2 for the libc-built Redis | med |
| N60 | node | warm-huge + 8 MiB cold purge, measured on the legacy node row | med |
| N61 | g++ | adaptive slab + 128 KiB local budget for >=2 KiB classes | med |
| N62 | python | N1 span release + N24 page purge | med |
| N63 | git | N2 + N12 + N31 | med |
| N64 | sqlite | N2 + N12 + N24 | med |
| N65 | all | combine warm-huge + G3 + N23 + N26 | med |
| N66 | gcc/g++ | combine N44 (64 KiB obstack window) + N26 (64 KiB-1 MiB window) | low |
| N67 | python | combine N1 (span hint release) + N24 (2-page purge) + N23 (dead large span) | med |
| N68 | git/sqlite | combine N2 + N12 + N31 + N57 for startup mappings | med |
| N69 | large/cross | combine N37 (large-class FIFO) + N16 (cross-free batching) | med |
| N70 | sort | combine N45 (8-16 MiB purge) + warm-huge | low |
| N71 | redis | combine N43 (exact first span) + N59 + N2 | med |
| N72 | node | combine N60 (warm-huge + 8 MiB) + N44 (obstack/medium window) | med |
| N73 | g++ | combine N11 (adaptive slab) + N25 (128 KiB budget) + N61 | med |
| N74 | sort/xz | combine N29 (THP slab) + N39 (32 MiB huge cap) + N54 | med |
| N75 | micro | combine N5 (no prefetch) + N15 (class fold) only if both win | low |
| N76 | all | N8 128 KiB spans + fix sort +1.6% by keeping 64 KiB purge units | high |
| N77 | all | adaptive large-cache budget from RSS high-water instead of mapped bytes | high |
| N78 | sort/xz | MADV_FREE (N28) + warm-huge | low |
| N79 | all | idle-release timer (default off) + cold purge on idle, not on free | high |
| N80 | all | per-class page purge + earlier fully-free span release (N1 + N21) | med |
| N81 | python | confirm N67 (N1+N24+N23) at 11 reps; only PR if RSS down and no time regression | med |
| N82 | redis | combine N9 (lazy bump run) + N59 (exact first span + full slot) | med |
| N83 | node | confirm N72 (warm-huge + 8 MiB cold) on the legacy node row at 11 reps | med |
| N84 | gcc/g++ | combine N66 (64 KiB/medium windows) + N53 (THP slab after 2nd refill) | med |
| N85 | git/sqlite | combine N58 (startup mapping) + N32 (lazy dense) + N9 (lazy bump) | med |
| N86 | python | combine N67 + N50 (2-page purge below 256 B) | med |
| N87 | git | combine N58 + N31 (one-block first span) | med |
| N88 | sqlite | combine N58 + N24 (2-page purge) | med |
| N89 | micro | confirm N5 (prefetch removal) at 11 reps, including rand/cross micro | low |
| N90 | large/cross | combine N37 (large-class FIFO) + N16 (cross-free batching) | med |
| N91 | redis | combine N9 + N59 + N60 (8 MiB cold purge) | med |
| N92 | node | combine N72 + N45 (8-16 MiB purge) | med |
| N93 | g++ | combine N66 + N11 (adaptive slab) + N25 (128 KiB budget) | med |
| N94 | sort | combine N74 (32 MiB huge cap + THP slab) + N53 | med |
| N95 | xz | combine N74 + N28 (MADV_FREE) | low |
| N96 | all | after the current batch, combine the top 3 measured winners into one branch | med |
| N97 | all | adaptive large-cache budget from RSS high-water, not mapped bytes | high |
| N98 | git/sqlite | combine N58 + N2 + N12 | med |
| N99 | python | confirm N67 + N86 at 11 reps | med |
| N100 | redis | combine N43 + N9 + N59 | med |



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

## RSS wave 1 (2026-11-24)

| id | branch | result | status |
|---|---|---|---|
| RSS0 | `exp/rss-balanced2` | cold large-cache purge in balanced defaults: sort RSS 45 -> 35 MiB (11 reps), jq/Python -1 to -2%, but jq time +1.8%, Python +1.4% | **candidate**, needs a size threshold |
| RSS2 | `exp/rss-sort-cache` | custom cold purge: sort RSS flat, time neutral | rejected |
| RSS3 | `exp/rss-gxx` | 32-128 KiB warm cap 8 MiB: g++ RSS 199 -> 199, no effect | rejected |
| RSS4 | `exp/rss-startup` | 4 KiB first spans: git 13 -> 16, sqlite 13 -> 16, worse | rejected |
| RSS6 | `exp/rss-python` | 128 KiB-1 MiB cap 4 MiB: jq RSS -1.7% but jq +2.5%, sort +2.2%, sqlite +1.6% | rejected |
| RSS7 | `exp/rss-close` | jq is within noise (tied); xz +0.68% is structural, every one-line knob is a no-op | jq tied, xz needs RSS8 |
| FIX1 | `fix/foreign-mus` | Redis 8 + jemalloc crash in `malloc_usable_size`; magic-check + `dlsym(RTLD_NEXT)` fallback; merged as PR #2 | **merged** |

The lesson so far: the RSS gap is **not** the large-cache byte cap.  It is
resident pages of cold/huge mappings (cold purge works), plus structural
startup mappings (smaller spans made it worse).  `RSS8` (per-app
smaps/massif decomposition) is the next measurement, and the size-band
threshold on RSS0 is the next code step.

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
