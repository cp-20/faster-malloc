# Redesign study: page-level ownership and decay for v2 peak RSS

*Design research only. No allocator code, benchmark harness or Makefile is
changed by this document. Status: proposal, not an ADR.*

## 1. Question and evidence base

v2 now wins wall time on most apps (ADRs 0079–0082, PR #3) and PR #3's cold
large-cache purge cut `sort` peak RSS by ~20%. The remaining peak-RSS gaps
to the best competitor are large: git +15%, sqlite +27%, redis +16%, node
+17%, gcc +7%, Python +4%. The question is whether an architecture change
can close the *large-process* gaps (redis, node, gcc, Python; sqlite OLTP is
a short/large mix) **without giving back the exact-instruction time wins**.

Facts taken from `src/fm_core.h` and `src/size_classes2.h` on this tree:

| fact | value |
|---|---|
| span geometry | `SPAN_SIZE = 65536`, `SPAN_MASK = ~(SPAN_SIZE-1)`, `HEADER_SIZE = 64`, `FM_PAGE_SIZE = 4096` |
| classes | `CLASS_COUNT = 145`, `SMALL_MAX = 32768`, `ARITH_MAX = 1024` |
| span header (`FM_Span`) | exactly 64 B: `cls/magic/nblocks/dead_mark` (16), `map_base/map_size/user_size/align/lnext` (40), `purge_mark/purged_mask/withheld_cnt` (8) |
| free fast path | `andq $-65536` → one `sp->cls` load → TLS push asm; frameless, ~17 Ir (ADR 0004/0078) |
| census slot key | `span >> 16` (`fm_census_slot`, prefetch sites at 1381/1386/1726/1731) |
| page purge (ADR 0030) | per-span 16-bit `purged_mask`, arrays `page_free/page_tot/page_sfree/page_stot[16]` in `FM_CensusEnt`, `mincore` gate, coalesced `MADV_DONTNEED`, withheld-block re-carve |
| release ageing (ADR 0020) | `dead_mark` + `FM_RELEASE_AGE_MS=100`, `FM_RELEASE_KEEP=2`, `FM_RELEASE_EVERY=16`, interval 10–5000 ms, driven by *flush events* |
| mapping sources | 2 MiB dense extent (32 spans), 4 MiB per-thread large slab (64 carves), `FM_SMALL_FIRST_SPAN` 16 KiB carved value in a 64 KiB VA slot |
| large cache | 48 buckets, hot window 8 (64 warm), 64→128 MiB warm budget, cold `MADV_DONTNEED` only for buckets ≥ 24 (≥16 MiB, PR #3) |
| idle maintenance | `fm_idle_thread_main` exists but is **opt-in** (`FM_IDLE_RELEASE=0`); 200 ms tick, 1 s silence, full `fm_trim_class` sweeps |
| fork/JIT coupling | `fm_fork_prepare/parent/after_fork_child`, `fm_atfork_ready`, NODELETE requirement (ADR 0040) |

Already rejected, and therefore treated as constraints rather than options:

* **Cache/byte knobs are exhausted** (X3, X11–X14, RSS3/RSS6, `rss-sort-cache`).
  The gap is *resident pages of mappings*, not cache capacity.
* **Startup mapping/shrinking variants fail** (N2/N12/N31/N58/N85/N88/N91/N98,
  `rss-startup`): 4 KiB first spans made git/sqlite 13→16 MiB. The lesson in
  `allocator-rss-research.md` is "avoid spans, not shrink them".
* **brk bootstrap is unsafe** (N101 cross correctness, N117 blocked): glibc's
  own brk and the multithreaded cross test rule it out.
* **Locality/grouping on the hot path** costs Ir (X1/X2), so any option that
  adds a dependent load to `malloc`/`free` starts with a large handicap: the
  entire time advantage of v2 rests on ADRs 0002/0004/0078/0079.

## 2. Mechanism-level diagnosis

The 64 KiB mask is not itself a waste of RSS — an unused tail of a span is
VA-only, and ADR 0030 already purges whole 4 KiB pages of a *partially* free
span. The two real defects are:

1. **The release/purge decision is event-driven, not time-driven.** `fm_auto_release_tick`
   runs only from `fm_flush_bin` (every `FM_RELEASE_EVERY` flushes). A process
   that frees a lot and then computes (sort merge, cc1plus obstack pause,
   Python phase change, Redis idle) keeps the pages until the *next* free
   triggers a census. Dead marks can age for seconds while nobody censuses.
   The research note names this as the missing piece; jemalloc's
   `dirty_decay_ms`/`muzzy_decay_ms` and tcmalloc's
   `ReleaseToOSIntervalMs` are the reference implementations.
2. **There is no muzzy state.** ADR 0030 uses only `MADV_DONTNEED`; the
   mapping is either hot or purged, with no cheap "give the page back but
   keep the VA and avoid a re-fault/munmap cycle" step. That is what makes
   re-use after a pause expensive, and it is why eager release (ADR 0014)
   failed on cyclic patterns.

The large-process gaps are consistent with (1)+(2): redis/node are
long-running and retain empty pages across request bursts; gcc/Python have
distinct phases with dead mappings between them (ADR 0077 measured 6.8 MiB of
idle freed mappings before `dumps`/`loads` even with the cache hot). git and
sqlite are ~17 ms processes that never reach a decay window — they are a
*structural* one-heap-vs-many-mappings gap, and 12 prior startup variants
failed to close it. This document does not claim to close git/sqlite; `-20%`
on sort and the short-process gap must at minimum be preserved.

## 3. Options

### Option 1 — 16 KiB spans for small classes, page→span map instead of the mask

Replace `SPAN_SIZE=64 KiB` with 16 KiB for small classes and replace
`sp = p & SPAN_MASK` with a global page-indexed directory. Two sub-variants
matter:

* **1a, 16 KiB-aligned spans + 16 KiB mask**: no page map; `SPAN_MASK` changes
  to `~0x3FFF`. Cheap but yields 4× the spans, 4× the headers on their own
  pages, 4× the census entries and 4× the mapping/carve traffic — exactly the
  "more spans" failure mode of `rss-startup`. It also cannot serve the
  16 KiB class (`HEADER_SIZE + 16384 > 16384`), so classes ≥ 8 KiB must stay
  64 KiB. No dense packing benefit: the remainder of each 64 KiB slot remains
  VA-only either way.
* **1b, page map proper**: spans placed on arbitrary 4 KiB boundaries so
  small classes consume exactly the pages they touch. A 4-level radix over
  4 KiB pages (9/9/9/9 bits, 8 B leaf) costs a 4 KiB top table plus one 4 KiB
  leaf per 2 MiB of covered VA: ~1 MiB of metadata for a 512 MiB heap
  (~0.2%), of which only touched leaves fault in.

| dimension | assessment |
|---|---|
| invariants | every 4 KiB page maps to at most one span; a span occupies a whole number of pages and one class; leaf entry stable for the span's life (update on class reuse under a lock) |
| fast-path cost | **1b adds 2 dependent loads** (radix L1 → leaf → `sp->cls`) plus range/alignment handling; ~+6–10 Ir per free. mimalloc's `cross` is 43–62 ns vs v2's 34 ns (ADR 0078), and X1/X2 show the micro gate is unforgiving. 1a keeps 1 AND + 1 load. |
| free-path cost | asymmetric: 1b pays the radix walk on *every* free; 1a unchanged. A reserved-window range check can hide it for hot classes only (that is option 4). |
| metadata | ~1 MiB radix per 512 MiB mapped (1b) + 4× `FM_Span` headers and 4× `FM_CensusEnt` churn vs today (1a/1b) |
| migration from `fm_core.h` | regenerate `size_classes2.h`; change every `& SPAN_MASK` / `>> 16` site (`fm_free`, `fm_malloc` debug, `fm_refill_class`, `fm_regroup_by_span`, `fm_census_slot`, 4 prefetch sites, `fm_is_aligned_ptr`, `fm_usable_size`, `fm_realloc`); shrink `page_*[16]`→`[4]`; add the radix and its lock; split `fm_map_dense_span` carve size; keep a 64 KiB path for large/aligned. High blast radius. |
| RSS gap closed | at best Python/gcc page-granular retention; **git/sqlite likely worse** (more spans) per the measured `rss-startup` result. No time budget to pay for it. |

### Option 2 — mimalloc-style segments/pages, per-page free lists, abandoned-page migration

Introduce `FM_Segment` (4/32 MiB, page bitmap, owning heap) and make each
`FM_Span` a page with an owner heap and an atomic foreign-free stack. A
cross-thread free that lands on a foreign page pushes to that page's
foreign-free stack (or abandons the page to the freeing thread's heap); the
owner drains it on its next visit ("page migration"). Empty pages return to
the segment and can be re-carved for a different class. Delayed `MADV_FREE`
purge and whole-segment retirement come with the model.

| dimension | assessment |
|---|---|
| invariants | page ↔ exactly one segment and one owner heap (owner may change via abandonment); a page's primary free list is touched only by its owner; foreign frees are visible only via the atomic stack until drained; a segment dies only when all its pages are empty |
| fast-path cost | malloc pop is comparable to today (page-local list from current segment), but free needs a page lookup (map or mask) **plus an owner check**; foreign path adds a CAS and ownership transfer |
| free-path cost | same-thread free ≈ v2 + owner load; cross-thread free materially more expensive than v2's single TLS push. `micro cross`, `threads` and `mtrepeat cross` are the sensitive cells; the rejected `exp/dense-thread` already failed `micro cross` on a lighter ownership change. |
| metadata | page headers (~64 B/64 KiB = 0.1%), segment bitmaps (64 pages → 8 B/segment), page map if pages are variable-sized (~1 MiB/512 MiB), per-thread abandoned lists (8 B/page) |
| migration from `fm_core.h` | `FM_Span` gains `owner`/`foreign_head`; `FM_Segment` is new; `fm_span_new`/`fm_map_dense_span` become segment page allocators; `fm_refill_class` asks the current segment; `fm_free` routes foreign blocks; `fm_central` stack and the census/regroup machinery are replaced or duplicated by page-local lists and a reaper. This is a rewrite of refill/flush/census, not a change. |
| RSS gap closed | redis/node (empty-page reuse and segment retirement under cross-thread churn), Python (thread-exit page return). High time risk on the cells that protect the wins. |

### Option 3 — jemalloc-style extents + dirty/muzzy decay **(recommended)**

Keep 64 KiB-aligned spans, the mask, the TLS arrays and the census exactly as
they are. Treat each existing span as an *extent* and add a time-based state
machine on top of the machinery that is already present:

```
ACTIVE ──(census finds fully free / purgeable pages, mark ages)──> DIRTY
DIRTY  ──(dirty_decay elapsed)──> MADV_DONTNEED pages ──> MUZZY
MUZZY  ──(muzzy_decay elapsed)──> munmap / return to dense-extent free run
```

`dead_mark` and `purge_mark` are the existing dirty marks; add one
`uint32_t muzzy_mark` (grow `HEADER_SIZE` 64→128 B, still inside page 0 so no
extra resident page, 0.1% fewer blocks per span, or pack the state into the
high bits of the marks). Add intrusive dirty/muzzy queues reusing the
otherwise-free `lnext` field of small spans, plus a small open-addressed
extent hash keyed by `span >> 16` (≈2× active spans × 16 B ≈ 128 KiB for 4096
spans) used only to coalesce *adjacent fully-free extents* in stage 2. Decay
pulses run in the existing `fm_idle_thread_main`, under the existing
`fm_census_lock`/`fm_central_lock`, with a per-pulse span budget so
application threads are never blocked long. Arm the thread lazily only above
a mapped-bytes / large-free high-water, so 17 ms git/sqlite and the micro
benchmarks never create it.

| dimension | assessment |
|---|---|
| invariants | an extent is 64 KiB-aligned and belongs to one class while active; DIRTY extents hold no live blocks; MUZZY extents hold no live blocks and have no resident payload pages, but keep their VA and header; a coalesced run only combines extents of the same arena/state; all transitions happen under the census lock |
| fast-path cost | **zero** — no change to `fm_malloc`, `fm_free`, `fm_tls_push_free`, `fm_class_of`, or span geometry; exact Ir parity is expected |
| free-path cost | zero for small blocks; large-free cold purge stays as PR #3 (the muzzy `MADV_FREE` step is a *later* transition on a different call site, not on the admission path) |
| metadata | +4 B/span (muzzy mark) + intrusive queues (0) + extent hash ~128 KiB per 4096 spans ≈ **0.05–0.1% of mapped bytes** |
| migration from `fm_core.h` | localised: extend `FM_Span` and `FM_Config` (two decay intervals, high-water, pulse budget); extend `fm_idle_kick`/`fm_idle_thread_main` to run budgeted pulses and the muzzy transition; add the extent hash next to `fm_shared_census`; keep `fm_trim_class` as the primitive. Reuse `fm_fork_prepare/parent/after_fork_child` and ADR 0040's NODELETE handling unchanged. |
| RSS gap closed | redis/node (steady-state baseline drops between bursts), gcc/Python (dead pages released during compute phases and after thread exit); preserves sort's purge; git/sqlite unchanged (no regression, no cure) |

### Option 4 — hybrid: 64 KiB for hot small classes, 16 KiB/page-map for cold or large-process classes

Keep the mask for the classes that dominate the fast path (≤256 B: jq DOM,
Python objects, refills in `fm_refill_class`). Put "cold" classes (sparse
large-small classes, or all classes once a mapped-bytes high-water says
"large process") into a reserved 16 KiB-span arena whose ownership is held in
a page map. `fm_free` selects the path with a **range check** against the
reserved arena (`p >= cold_lo && p < cold_hi`), predicted false on hot apps:
two compares and a branch, no added load on the hot path. Cold frees pay the
radix lookup.

| dimension | assessment |
|---|---|
| invariants | every pointer belongs to exactly one of the two arenas; the reserved VA window is registered before its first span is handed out; a class is never migrated between arenas while a span of it is live; page-map leaves are updated under the central lock on class reuse |
| fast-path cost | +2 compares + 1 well-predicted branch on free (1a-style mask unchanged for hot); may be measurable in exact Ir even when correctly predicted, so it is a *stage-2* candidate only |
| free-path cost | cold frees pay a page-map lookup; cross-thread cold frees additionally need the leaf lock |
| metadata | 4-level radix over the cold window only: e.g. 1 GiB window → 512 leaves ≈ 2 MiB VA, resident leaves ~0.1–1 MiB; plus dual span headers, dual bump pointers, dual central paths |
| migration from `fm_core.h` | all 1b sites **plus** a second mapping path, a class→arena policy in `fm_config_init`, dual `fm_bump_ptr/end`/`fm_tls` handling, and a range check in `fm_free`/`fm_realloc`/`fm_usable_size`/`fm_is_aligned_ptr`. Largest patch of the four. |
| RSS gap closed | in principle git/sqlite startup (compact cold spans) and large-process small-class tails; but the startup mechanism is contradicted by `rss-startup`, and the large-process path is a slower version of option 3's page purge. |

## 4. Recommendation

**Adopt option 3, in two stages.** It is the only option that changes RSS
without touching a single instruction of the measured hot path, and it
reuses machinery already present in `fm_core.h` (dead marks, purge marks,
`fm_trim_class`, the idle thread, the fork hooks). Options 1 and 2 attack the
right symptom but tax `free`/cross-free, which is precisely what the
adoption gate in `GOAL.md` protects; option 4 inherits both their costs and
the already-refuted startup-span theory.

Staged implementation:

**Stage 1 — measurable, one branch (`exp/dirty-muzzy-decay`).**
1. Add `muzzy_mark` to `FM_Span` and `dirty_decay_ms` / `muzzy_decay_ms` /
   `decay_high_water` / `decay_pulse_spans` to `FM_Config` (env overrides,
   lazy `fm_config_init`, no hot-path read).
2. Extend `fm_idle_kick` to arm the existing maintenance thread only when
   mapped bytes or large frees cross the high-water. Keep
   `FM_IDLE_RELEASE=0` semantics for the external knob; the internal gate is
   the default, so short apps and the micro suite never spawn a thread.
3. Run budgeted `fm_trim_class` pulses in `fm_idle_thread_main`: a pulse
   marks/censuses at most `decay_pulse_spans` extents per class, then yields.
   Fully-free extents transition DIRTY→MUZZY by `MADV_DONTNEED` after
   `dirty_decay_ms`; the muzzy transition after `muzzy_decay_ms` either
   `munmap`s or returns the extent to the dense-extent free run.
4. Leave PR #3's cold-large-bucket `MADV_DONTNEED` on the free path exactly
   as is; the muzzy `MADV_FREE` experiment is a separate, later hypothesis.
5. Acceptance: all cells in §5 green. Stage 1 must be Ir-identical on the
   micro suite and must preserve sort/jq RSS.

**Stage 2 — optional, only if stage 1 leaves a measured large-process gap.**
Add adjacent-extent coalescing and class reuse via the extent hash (still no
hot-path change), then the muzzy `MADV_FREE` variant behind its own cell.
Only if RSS8 (`smaps`/`mincore` decomposition, still unclaimed) shows the
residual is *small-class span granularity* should option 4 be prototyped,
behind the arena range check, and only if the two extra compares are within
`micro_worst <= 1.01`.

Explicit non-goal: git/sqlite startup RSS. Decay needs seconds; those
processes live ~17 ms. Twelve startup variants failed; closing that gap needs
the RSS8 decomposition and a separate single-heap-like mechanism, not a page
map.

## 5. Benchmark cells that decide it

Run on CI only (`bench/ci/expbench.py` / `exp-legacy.yml`), never locally.

**Gate A — time wins preserved (stage 1 must be exact):**
* `score.micro_geomean <= 1.00`, `score.micro_worst <= 1.01`; exact `Ir/op`
  from `bench/instr` for `churn8`, `churn1024`, `rand`, `cross`, `batch1024`,
  `realloc`, `large`, `threads`. These are the cells ADRs 0078/0079 moved and
  that X1/X2/X7 broke; any non-1.0000 result here rejects options 1/2/4.
* `score.real_paired_geomean <= 0.99`, `score.real_paired_worst <= 1.02`.
  Watch `sort` (~17 ms) and `git` (~17 ms) specifically: if the decay thread
  arms in a short app, its sleep/lock/fork cost will show up first in
  `real_paired_worst`.

**Gate B — RSS moves in the right direction:**
* `exp-legacy.yml` must first be re-baselined on current main (backlog LGC1)
  for `redis`, `sqlite1`, `jq_legacy`, `lua`, `python_legacy`, `node`, `gcc`,
  `git_legacy`. These rows are stale (rounds 5/6/10) and are the only source
  for the +16% redis / +17% node / +7% gcc gaps.
* New suite peak RSS for `python_data` (+4% gap), `gxx_compile` (+3.5%),
  `sqlite_oltp`, `jq`, `xz`, `sort`, `git_revlist`. Adoption rule: **no app
  that moves may be worse than main**, and at least one must move closer to
  `v2 RSS <= best competitor`. In particular `sort` must stay at PR #3's
  ~-20% level and `jq`/`xz` must not regress.
* Decisive stage-1 cells: `redis` peak RSS, `node` peak RSS, `gcc` peak RSS,
  `python_data` peak RSS, with `sort` RSS as the guard. If redis/node do not
  drop while gcc/Python do, the decay window is too long for the benchmark —
  shorten `dirty_decay_ms`, not the hot path.

**Gate C — safety:**
* correctness gates pass (`mtrepeat cross`, thread-exit drain, lifecycle/JIT,
  fork child); `abicheck` advisory as on main;
* no new mapping from `exp/dense-thread` (`micro cross` rc=1) or N101
  (`mtrepeat cross` rc=1) — i.e. no ownership or brk change in stage 1;
* RSS decomposition artifact (RSS8) captured alongside the run, so stage 2
  is chosen from data rather than from another knob sweep.

If gate A fails, reject the branch regardless of RSS. If gate A passes and
gate B is flat, record "no" with numbers and keep the idle thread opt-in;
do not bundle stage 2 into the same branch (GOAL: one hypothesis per branch).
