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
| N101 | startup | brk-based bootstrap arena for the first 1-2 MiB of small spans; switch to mmap after the threshold | high |
| N102 | all | `madvise(MADV_DONTNEED)` fully-free spans instead of `munmap`; keep the VA slot | med |
| N103 | redis | combine N82 + N91 + N100 (all Redis startup variants) | med |
| N104 | git/sqlite | combine N98 + N85 + N88 (all startup mapping variants) | med |
| N105 | node/xz/g++ | combine N92 + N95 + N84 (all large-buffer/obstack variants) | med |
| N106 | python | combine N67 + N80 + N99 (all Python RSS variants) | med |
| N107 | startup | 8 KiB first spans + N2 full-slot cap (avoid creating more spans) | med |
| N108 | startup | 32 KiB first spans + N2 full-slot cap | med |
| N109 | sort/xz | dense-extent THP hint only after 8 extents instead of 4 | low |
| N110 | all | per-class large-cache budget for small classes (<=8 MiB buckets) | low |
| N111 | all | keep the 64-byte span header on its own cache line without growing the header | low |
| N112 | large/cross | hybrid LIFO for small classes and FIFO for large classes (N37 refined) | med |
| N113 | sort | combine PR #3 cold purge + N76 128 KiB/64 KiB purge unit | med |
| N114 | startup | combine N58 + N63 + N68 | med |
| N115 | all | idle-release timer + cold purge on idle (N79 refined) | high |
| N116 | all | RSS high-water adaptive per-class local limits | high |
| N117 | startup | brk arena for the first 64 small spans, then the dense extent | high |
| N118 | all | MADV_FREE for fully-free spans instead of MADV_DONTNEED | med |
| N119 | all | malloc_trim-like pressure release every 64 large frees when RSS is above high-water | med |
| N120 | all | combine the best measured RSS mechanisms into one branch, then run the legacy matrix | med |
| N121 | all | implement Stage 1 of docs/exp/redesign-page-map.md: `muzzy_mark`, dirty/muzzy decay intervals, high-water lazy idle-thread arming, budgeted `fm_trim_class` pulses; no hot-path change | high |
| N122 | all | Stage 1 tuning on top of N121: dirty 100 ms / muzzy 1 s / 64 MiB high-water / 64-span pulse | med |
| N123 | all | Stage 2: adjacent fully-free extent coalescing + class reuse via the extent hash (still no hot-path change) | high |
| N124 | sort/xz | muzzy `MADV_FREE` variant of N121 after the DONTNEED stage is measured | med |
| N125 | all | add `FM_DIAG_RSS=1` mincore/smaps-classified residency counters for the allocator's own mappings (diagnostic only, no bench edits) | low |
| N126 | git/sqlite | RSS8 follow-up: use the N125 decomposition to design a single-heap-like startup mechanism; do not guess | high |
| N127 | all | combine Stage 1 N121 + N105 + PR #3 after Stage 1 is measured | med |
| N128 | all | Stage 1 tuning on `exp/auto-n121`: high-water 64 MiB, dirty 100 ms, muzzy 1 s, pulse 64 spans | med |
| N129 | sort/xz | muzzy `MADV_FREE` variant of N121 after the DONTNEED stage is measured | med |
| N130 | all | combine N121 (decay) + N123 (extent hash / VA slot reuse) | med |
| N131 | all | N121 with a mapped-bytes high-water instead of cumulative large frees | med |
| N132 | all | use N125 `FM_DIAG_RSS=1` decomposition on redis/node/gcc/python to pick the next decay target | low |
| N133 | all | aggressive decay for benchmark-length runs: high-water 16 MiB, dirty 10 ms, muzzy 100 ms, pulse 64 | high |
| N134 | all | flush-triggered decay: purge DIRTY spans on the next flush after 10 ms, no idle thread | high |
| N135 | all | page-map/segment rewrite spike only if decay tuning fails; measure the free-path Ir cost first | high |
| N136 | all | dedicated CI run of N125 `FM_DIAG_RSS=1` on redis/node/gcc/python to attribute the resident pages | low |



## Round 16: large-cache budget and span geometry (2026-11-25)

| id | branch | result | status |
|---|---|---|---|
| L2 | `exp/lcache16b` | balanced large-cache budget 64 -> 16 MiB (warm cap 32): legacy node RSS 515 -> 460 MiB, python 447 -> 447, gcc 365 -> 365, redis 6.2 -> 6.2; 11-rep exp-bench micro 1.0000, paired 0.9972, worst 1.0000 | **accepted** |
| L3 | `exp/lcache32b` | same at 32 MiB: node 470 MiB; 11-rep micro 1.0000, paired 1.0013, worst 1.0075 | alternative |
| L4 | `exp/lcache8b` | same at 8 MiB: node 455 MiB; two 11-rep runs pair sqlite at 1.0168 / 1.0224 while 16 MiB is neutral | rejected (time) |
| B1 | `exp/first-batch` | first refill takes 1 block, then 4, then the limit: git 8.43 -> 8.36 MiB, no micro change | rejected |
| T1 | `v7/slab-thp` | adaptive THP on the >4 KiB slab, 2 MiB-aligned extent: 11-rep neutral (micro 1.0000, RSS flat); THP never fires in the real apps | rejected |
| T2 | `v6/c` | 4 KiB cold spans in 64 KiB slots + always-THP hot chunks: jq tied, git/sqlite 13 -> 16 MiB | rejected |
| G1 | `v6/slot-hybrid` | 4 KiB spans in 64 KiB slots + 8-refill promotion: micro 0.9996, jq +10%, git 14 MiB | rejected |
| R1 | `exp/mremap-grow` | in-place `mremap` on large realloc: correct, RSS flat (adjacent VA occupied); `MREMAP_MAYMOVE` moves the block off the 64 KiB span mask | rejected |
| R2 | `exp/mremap-fixed` | reserve a 64 KiB-aligned destination, then `mremap(MREMAP_FIXED|MAYMOVE)`: RSS `grow` 139.7 -> 132.8 MiB and python JSON 142.0 -> 138.0 MiB, but python is **+15% locally and +2.2% at 11-rep CI**; sort +6.1% | rejected (time) |
| D1 | div2 screen | `FM_LOCAL_LIMIT_DIV=2`: git/sqlite 13 -> 12 MiB but micro 1.0239 and jq +2.5% | rejected as default |

Two measurements that correct earlier conclusions:

* the large-cache **byte cap is** the dominant RSS lever for Node.  The RSS
  wave 1 apps (jq, sort, g++, Python, xz) never exercised the multi-MiB
  free-and-forget band, so the knob looked inert.  Node's legacy row moved
  527 -> 468 MiB with a 16 MiB budget at zero time cost.
* v3's 4 KiB-span time losses were an artefact: `size_classes3.h` set
  `SMALL_MAX` to 3968 while keeping 145 classes, so every 3969..32768 B
  request fell into the large path.  Live 4-24 KiB allocation measured
  4.4-4.8 s against v2's 0.04-0.57 s.  Span geometry experiments on that
  header (v3, v4, v6) are therefore not evidence about 4 KiB spans.

| M1 | measurement | `livepeak` LD_PRELOAD wrapper: peak live bytes through malloc are **11.65 MB (glibc) vs 11.81 MB (v2)** on the legacy git fixture while RSS is 19.6 vs 23.9 MB.  The gap is page fragmentation across size classes, not rounding and not whole-block retention. | evidence |
| M2 | measurement | Lua churn on the legacy workload (local, 11.6 vs 16.4 MB) decomposes into lcache 1.7 MB, eager release 1.1 MB, div16 0.9 MB, page purge 0.6 MB; only the combination reaches 13.7 MB, so no single knob closes it. | evidence |
| M3 | `exp/warm64` | warm-bucket cap 64 MiB with a 16 MiB base: node 468 MB at 3 reps, identical to the 32 MiB cap; the cap is not the constraint | rejected |
| M4 | lcache8 re-run | 8 MiB base at 11 reps incl. xz: micro 1.0000, paired 1.0012, worst 1.0168 (sqlite **+1.7%**, second independent run +2.2%); node RSS 466 vs 471 at 16 MiB.  Time is the hard constraint, so 16 MiB stays. | 8 MiB rejected |

Still open from this round: a phase-reuse test (`mixpack` mode 1) shows v2
retaining freed spans across a size-phase change (166 MB vs glibc 119 MB)
until `FM_RELEASE_AGE_MS=0` releases them (118 MB); git does not respond to
the same knob, so its +4 MiB is not dead spans.  Fragmentation under churn
is *better* than glibc (709 vs 702 MB at 1M live), and simultaneous
mixed-size packing is better (224 vs 237 MB), so the git gap is isolated
to its pack/delta buffers, not the general small-object path.

## Round 17: cross-class span recycling (2026-11-25)

| id | branch | result | status |
|---|---|---|---|
| M5 | A/B | same branch (`exp/lcache16b-a`), node only, 11 reps, budget switched by `FM_LCACHE_MAX_BYTES`: 64 MiB = time +4.4% / RSS +10.1% vs glibc; 16 MiB = +7.3% / +6.0%.  The budget trades ~2.9 points of node time for 4.1 points of RSS; node is not green either way. | evidence |
| P2 | `exp/adaptive-cache` | shrink the large-cache budget to 2 MiB when the observed hit rate is below 1/8 (`fm_lc_hits`/`fm_lc_misses`): legacy node 468164 KB, python 457608, gcc 374360, redis 6416 -- identical to the plain 16 MiB budget, so the branch never fires on these apps | rejected (no effect) |
| P1 | `exp/span-pool` | a fully free span keeps its 64 KiB mapping (pages `MADV_DONTNEED`-ed) in a per-thread pool and any class re-carves it.  `strace -c` on the Lua workload: munmap 204 -> 9, madvise 54 -> 222, mmap 57 -> 51, syscall time 2.63 -> 1.10 ms.  RSS: git 23.9 -> 23.9, lua 16.2 -> 15.9, sqlite speedtest1 11.5 -> 12.3 MiB; runtime unchanged on all three. | rejected (RSS neutral/worse, time within noise) |

The mechanism works as designed, but the measured residual is **partially
free** spans, not dead ones: the census page purge is a no-op on
git/lua/sqlite, and peak live bytes (11.81 vs glibc's 11.65 MiB on git)
are already within 1.4%.  A true 4 KiB cross-class page list would need a
page map and still cannot reclaim a page that holds one live block, which
is why mimalloc (26.9), jemalloc (27.0) and tcmalloc (30.0) all lose to
glibc's 22.6 MiB on the same git row.

## Round 18: byte-granular shared-page prototype (2026-11-25)

The residual RSS gap is page fragmentation **between size classes**, so a
size-agnostic page heap was prototyped and measured against the same
local workloads (lua churn, git rev-list, sqlite speedtest1).

| id | what | result |
|---|---|---|
| S1 | trace simulation: ideal byte-granular packing of the recorded lua and git allocation traces (16 B units, 8 B headers, perfect coalescing) | ceiling is **9.04 MB (lua)** and **12.04 MB (git)** against glibc heaps of 9.18 and 11.1 MB: the best possible shared-page design is glibc-level RSS, not lower |
| S2 | `/tmp/pm.c` prototype: 4 KiB pages, 8-byte chunk headers, size-agnostic free buckets with a non-empty bitmap, forward coalescing, glibc forwarding above 2032 B, tcache only for chunks <= 256 B | passes `test_basic`, `stress`, `rtest`, `valgrind`. **git 20.4 vs glibc 19.6 / v2 23.9 MiB at equal time; sqlite 8.5 vs 8.35 / 11.6 MiB at equal time; lua 11.9 vs 11.4 / 16.2 MiB but 0.72 s vs v2's 0.36 s** |
| S3 | tcache size sweep on lua | no tcache 11.8 MB / 0.78 s; 8/bucket 15.8 / 0.81; 16/bucket 20.0 / 0.77; 32/bucket 27.6 / 1.03; restricted to chunks <= 256 B 11.9 / 0.72 |

Conclusion: the shared-page design reaches glibc-level RSS on these apps
and, on git and sqlite, does so at v2-equivalent time.  Lua needs the hot
path optimised (its string churn hits the page heap 2x harder than v2's
bins).  The next cycle should integrate S2's small path into v2 behind a
flag and re-run the 11-rep matrices.

## Round 20: page-heap integration rejected by the 11-rep matrix (2026-11-25)

| id | branch | result | status |
|---|---|---|---|
| P3 | `exp/pageheap` | ADR 0086's 257-2032 B page heap, 11 reps: **micro 1.1844 (worst 1.559)**, jq 1.208 / RSS 262 -> 285 MiB, git 1.440, gcc 1.004 / RSS +2.4%, sort RSS +6.3%, sqlite RSS +3.2%.  Legacy 11 reps: **git 29.5 -> 26.7 MiB (-9.6%)** but node 471 -> 525, jq 368 -> 385, lua 16.6 -> 18.2, gcc 374 -> 379, sqlite 13.9 -> 14.0 | rejected (only git wins) |

| P4 | `exp/purged-cache` | keep the VMA of a large block that misses the byte budget, `MADV_DONTNEED` its payload, and reuse it from a per-bucket list: node munmap 6931 -> 3296 (madvise 1263 -> 4771) at cap 1024, node time 1.18 -> 1.15 s but RSS 405 -> 409 MB because each cached block keeps its header page; cap 256 is time- and RSS-neutral. | rejected |

Node's syscall profile (`strace -c -f` on `churn.js`): v2 does **6888
munmap + 1118 mmap** against glibc's 1632 + 960, for 0.44 s vs 0.22 s of
syscall time.  The freed sizes are overwhelmingly 64 KiB (4287), 128 KiB
(1006) and 256 KiB (789), i.e. the medium blocks just above `SMALL_MAX`,
which v2 carves from a 4 MiB slab and then `munmap`s one slice at a time.
That is the source of node's time gap and of the lcache budget trade
measured in Round 16.

The local A/B on `bench/apps/work/real_suite` had shown git 23.9 -> 21.1 MiB
at equal time and sqlite 11.6 -> 10.9, but both fixtures are much smaller
in CI (`exp-bench` git is a 13 MiB process) and the page heap's chunk
operations cost far more instructions than v2's bins: the micro geomean
rose 18% and jq, whose string buffers land in the band, lost 21% of its
time and 8.6% of its RSS.  Lesson: screen allocator-layout changes on the
paired 11-rep matrix, never on a single local fixture.

## Round 21: medium-block slab free-list (2026-11-25)

| id | branch | result | status |
|---|---|---|---|
| S4 | `exp/slab-free` | freed 32-512 KiB slab slices and carved interior pieces go to a per-thread free list instead of `munmap` (cap sweep on node): cap 0 munmap 6888 / 1.04-1.09 s / 401-405 MiB; cap 128 6725 / ~1.05 s / ~405 MiB; cap 512 5399; cap 4096 **4690 / 0.99-1.02 s / 437-463 MiB** | rejected |

The free-list does what it says -- node's 64 KiB munmap storm shrinks -- but
every retained slice is a resident page range, so the change is the same
RSS-for-time trade as the large-cache budget: cap 4096 buys ~5% of node's
time for up to 58 MiB, cap 128 is neutral.  The residual munmaps are
aligned allocations (V8 asks for `align=128` buffers) and dedicated
mappings above the slab cutoff, which cannot join the slab list.

## Round 22: selective large-cache retention (2026-11-25)

| id | branch | result | status |
|---|---|---|---|
| C5 | selective + page purge | on the selective build, `FM_PAGE_PURGE=1 FM_PURGE_MIN_PAGES=1` with a 1 ms census and age 0: lua 14.74 -> 14.76 MB, git 23.88 -> 23.92 MB (and 15.3 with age 1000).  Fully-free pages are still not the residual. | rejected |
| C4 | `exp/selective3` | warm bucket cap 4 -> 32 MiB (let node's 27 MB JSON payload in): node time +8.3% -> +6.0%, node RSS 454.6 -> 455.2 MiB, gcc RSS 358.2 -> 361.5, python unchanged.  The node time difference is inside run-to-run noise and the gcc cost is real. | rejected |
| C3 | `exp/selective2` | relax the cold-bucket rule to one block of any size (protect xz's 8 MiB buffers): gcc 351.6 -> 361.8 MB, python 138.2 -> 140.9, xz unchanged at 57.7 MB | rejected |
| C2 | `exp/selective-cache` | on top of the 16 MiB base (ADR 0083): warm buckets (a hit) may keep 4 MiB, cold buckets one block <= 256 KiB, exact per-bucket byte accounting.  11-rep legacy vs the 16 MiB build: python 446.9 -> **432.0** MiB, gcc 364.6 -> **358.2**, jq 358.9 -> **353.9**, node 460.2 -> **454.6**, lua 16.2 -> **14.7**, git 28.8 -> 28.2, redis 6.2 -> 6.3, sqlite 13.5 -> 13.7; time equal or better except node +8.3% | **PR #5** |

The flat budget could not distinguish a reused JSON buffer (node) from a
free-and-forget 1 MiB obmalloc arena (CPython); per-bucket warm/cold
retention can, and it recovers RSS on python and lua without the node
time cost that ADR 0083 traded away.

## Round 23: quarter local batch for >= 2 KiB classes (2026-11-25)

| id | branch | result | status |
|---|---|---|---|
| B2 | `exp/large-class-div` | RSS5: classes >= 2 KiB keep a quarter of the 64 KiB local byte budget (16 KiB).  11-rep legacy vs the selective build: gcc 366.8 -> 365.4 MiB, lua 15.0 -> 14.5, sqlite 14.0 -> 13.5, node 465.5 -> 461.4, redis 6.5 -> 6.2, python/git flat.  11-rep exp-bench: micro 1.0004/worst 1.0012, **jq paired 1.0291**, real paired 0.9958 | rejected as a stack (RSS win, but it costs the only green app 1.5-2.9% time) |

The threshold of 2048 was chosen so `churn 1024` stays out of the change;
the 1024 threshold measured gcc -2.8 MiB but moves that micro case.  The
mechanism is real and broad-based (-3.3 MiB across five apps) but it
narrows jq's lead, and jq is the only app that is green in both
dimensions, so it is not stacked onto PR #5.

## Round 24: the free-page reservoir is measured (2026-11-25)

The campaign had repeatedly concluded "page purge is a no-op" from knob
screens.  That was wrong: the purge was simply unreachable.  `strace -f`
shows **zero** `MADV_DONTNEED` calls because (a) `page_purge` is off in
the balanced preset and (b) `fm_release_maybe` returns early while a
class's central depth is below `release_min_depth` (default 16).

An instrumented build (`FM_DIAG_PURGE`) proves the census itself runs
thousands of times per git run (`fm_trim_class` calls: 1595 and 8555 in
the two processes) and, once made reachable
(`FM_PAGE_PURGE=1 FM_PURGE_MIN_PAGES=1 FM_RELEASE_MIN_DEPTH=1
FM_RELEASE_INTERVAL_MS=1 FM_RELEASE_AGE_MS=0`), the purge pass finds

```
DIAG trim=8582 cand=58 purged=58
```

i.e. **58 fully free 4 KiB pages = 232 KiB** in the whole git rev-list
run, and the measured RSS moves by exactly that: 23.88 -> 23.71 MiB at
unchanged time.

So a per-page free list that lets fully-free 4 KiB pages return to a
shared pool can recover at most ~0.2 MiB on git, not the 4 MiB gap.  The
gap is pages that each still hold at least one live block, which neither
a page-level free list nor a cross-class page pool can reclaim without
moving live objects.  This closes the question for git/lua/sqlite/redis.

## Round 25: allocation locality is not the lever (2026-11-25)

The remaining git/lua gap was attributed to partial-page fragmentation, so
the next hypothesis was allocation locality: regroup the central stacks
into span-pure batches more often so a class's live blocks cluster on
fewer pages.  The census is what does that regrouping, and its cadence is
`FM_RELEASE_INTERVAL_MS`; running it 50x more often is the cheapest test.

| case | default (50 ms) | 5 ms | 1 ms |
|---|---:|---:|---:|
| git rev-list | 23.94 MB / 0.04 s | 23.92 / 0.03 | 23.86 / 0.04 |
| lua churn | 16.29 MB / 0.32 s | 16.38 / 0.32 | - |

No effect at all.  Combined with Round 24 (only 58 fully free pages) and
the live-bytes measurement (11.81 vs glibc's 11.65 MB), this isolates the
residual to pages that each still hold at least one long-lived block:
regrouping, purging, pooling or re-ordering cannot reclaim them, and
moving live blocks is impossible without breaking user pointers.  The
only architectural answer is byte-granular pages, which the 11-rep matrix
rejected on the micro (Round 20).

## Round 26b: medium-bucket cold cap (2026-11-25)

| id | branch | result | status |
|---|---|---|---|
| C6 | `exp/selective7` | allow up to 512 KiB of cold retention in buckets 14-18 (16-512 KiB) to capture node's Buffer churn: local node munmap 7107 -> 6924, RSS and time unchanged | rejected |

Node's 64 KiB churn does not reach the cache insertion: the hot-window
gate (`FM_LCACHE_HOT_WINDOW` large frees since the bucket was last
allocated from) or the free-path preconditions reject it, so widening the
cold cap cannot help.  The remaining node time is the 27 MiB payload and
the munmap/mmap pairs that the window refuses, neither of which is a cap
tuning problem.

## Round 26: exempting >= 16 MiB buckets from the caps (2026-11-25)

Node's only time loss under the selective cache is its 27 MiB JSON payload
(bucket 24), which the warm cap of 4 MiB refuses to keep.  A variant
exempts buckets >= 24 from the warm/cold rule entirely, leaving the 16 MiB
base budget as the only bound.  Locally it is a no-op on jq, sort and xz.
11-rep exp-bench: micro 1.0002, jq RSS 252, real paired 0.9932 -- i.e.
identical to the selective build, as the 16 MiB base still rejects the
28.3 MiB block.  Legacy 11 reps: node 456.4 MiB / +7.3% time, i.e. the
same as the selective build (454.6 / +8.3%), and the other apps unchanged.
The variant that actually lets the payload in is `exp/selective6` (huge
buckets get a 32 MiB budget): exp-bench flat (micro 1.0005, jq 252, paired
0.9958), but its legacy is **worse than the selective build** -- node
462.9 MiB / **+10.0%** time against 454.6 / +8.3%.  Keeping the 27 MiB
payload buys 8 MiB of RSS and does not recover the time, so both huge-bucket
variants are rejected and PR #5 keeps the selective build.

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

The lesson so far: for these apps the RSS gap is **not** the large-cache
byte cap (Node later falsified this; see Round 16).  It is resident pages
of cold/huge mappings (cold purge works), plus structural startup
mappings (smaller spans made it worse).  `RSS8` (per-app
smaps/massif decomposition) is the next measurement, and the size-band
threshold on RSS0 is the next code step.

## Round 14 confirmations (11 reps and legacy)

| branch | result | status |
|---|---|---|
| `perf/warm-huge` (N3) | 11-rep paired 0.9979, sort +1.2%, no extra RSS | rejected |
| `exp/auto-n54` | 11-rep paired 0.9995, RSS flat | rejected |
| `exp/auto-n58` | 11-rep paired 1.0040, xz +2.1%, sqlite +0.8% | rejected |
| `exp/auto-g3` | 11-rep paired 1.0031, xz/Python +1.7% | rejected |
| `exp/auto-n10` | 11-rep sort +1.04%, same sort RSS as PR #3 | rejected |
| `exp/auto-n85` legacy | no RSS gain; sqlite 13.4 -> 15.1 MiB, git 29.7 -> 28.5 | rejected |
| `exp/auto-n91` legacy | no RSS gain; sqlite 13.4 -> 15.1 MiB, redis 6.4 -> 6.6 | rejected |
| `exp/auto-n101` | 11-rep correctness: `mtrepeat cross` rc=1 (brk arena vs glibc brk) | rejected |
| `exp/auto-n106` | 11-rep paired 0.9984, RSS flat, micro worst +1.49% | rejected |
| `exp/auto-n121` | 11-rep time neutral, micro 1.0011; legacy RSS unchanged (node 531, redis 6.4, gcc 374) | rejected as default; tuning N128/N131/N133 |

The lesson from round 14: 3-rep real-app wins are noise; adopt only
11-rep or full legacy evidence.  Startup mapping variants (N2/N12/N31/
N58/N85/N88/N91/N98) do **not** close the short-process RSS gap and can
make sqlite worse.  The brk-arena attempt (N101) is not safe with glibc's
own brk use and fails the multithreaded cross test; N117 is blocked for
the same reason.  Node/Redis large-buffer legacy measurements (N105/N103)
are the remaining RSS lead.

## Round 15: dirty/muzzy decay rejected

| branch | result | status |
|---|---|---|
| `exp/auto-n121` | 11-rep time neutral, micro 1.0011; legacy RSS unchanged (node 531, redis 6.4, gcc 374) | rejected |
| `exp/auto-n128` | 64 MiB/100 ms/1 s tuning: legacy RSS unchanged (node 528, redis 6.6, gcc 376) | rejected |
| `exp/auto-n131` | mapped-bytes high-water: legacy RSS unchanged (node 527, redis 6.36, gcc 374) | rejected |
| `exp/auto-n129` | MADV_FREE muzzy: peak RSS cannot improve before pressure | not pursued |
| `exp/auto-n133` | 10 ms aggressive decay: same mechanism, no RSS evidence | not pursued |
| `exp/auto-n134` | flush-triggered decay: same mechanism | not pursued |

The decay hypothesis is falsified on this workload set: the peak RSS gap is
not dirty pages that a time-based sweep can drop.  It is live allocation,
internal fragmentation, and the number of distinct mappings.  Together with
the rejected startup/brk/large-cache variants, every incremental RSS lever
has been measured.  Closing the remaining gaps (node +17%, gcc +7%,
sqlite +27%, git +15%, redis +16%, Python +4%) needs either the page-map/
segment rewrite (N135) with a taxed free path, or a separate single-heap
design (N126).  The campaign manager is paused at this decision point.

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
