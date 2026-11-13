# 0031 — Lua peak RSS: decomposed; live data is the minority

*Status: Accepted (2026-11-13). Contemporaneous. Round 6, item 2.
Closes ADR 0024's open lua item.*

## The question

Round 5 left two lua puzzles: the workload peaks at ~19 MB under
v2 against glibc's ~11.5 MB while its time is the best in the
field, and the footprint profile moved the peak in one paired
A/B but not in the full session run. ADR 0024's open item asked
whether the peak is live data or returnable retention.

## Method

`bench/apps/workloads/churn2.lua` is an instrumented fork of the
standing workload (identical allocation behaviour) that prints,
at each phase boundary, Lua's own live-byte accounting
(`collectgarbage("count")`) next to the process RSS from
`/proc/self/status`. Live data and retention are then separable
phase by phase instead of guessed from the peak.

## Results (KB)

| snapshot | lua live | glibc RSS | v2 RSS | jemalloc | mimalloc |
|---|---|---|---|---|---|
| p2 done | 1,485 | 6,232 | 8,196 | 8,452 | 6,424 |
| p3 built | 7,640 | 12,040 | 18,864 | 16,984 | 12,528 |
| post-GC | 7,640 | 12,104 | 18,648 | 16,984 | 12,528 |

At the peak, lua's live set is 7.64 MB — 40% of v2's RSS.
glibc's excess over live is 4.4 MB, mimalloc's 4.9, jemalloc's
9.3, v2's 11.2. The excess decomposes as:

- **~3.1 MB — large-cache growth trail.** Re-running with
  `FM_LCACHE_MAX_BYTES=0` drops the p3 peak from 18,648 to
  15,568 and changes nothing earlier. The trail is phase 3's:
  the `arr` table grows by realloc doubling (…400 KB, 800 KB,
  1.6 MB…) and each superseded block is cached, resident, by
  design (ADR 0011/0019 buy large-alloc reuse with exactly this
  retention). It is not phase 1's 2 MB concat strings — those
  are already recycled by p1's end.
- **~1.4 MB — baseline.** v2 starts at RSS ~4.0 MB where glibc
  starts at ~2.65 (library, span reserve, thread caches).
- **~6.7 MB — span residues of foreign classes.** Phase 2 churns
  tables through a handful of size classes; phase 3 allocates
  mostly strings (different classes) plus the sort's temporaries.
  In a span-segregated allocator, phase 2's partially-free spans
  cannot serve phase 3's classes, and the rolling window's live
  tables pin pages inside most of them, so neither whole-span
  release nor (round 6) page purge can return much of it during
  the run. glibc's unified arena recycles the same bytes across
  sizes, and its mmap threshold returns the growth trail
  immediately. jemalloc's 9.3 MB excess shows this band is what
  segregated designs pay here; mimalloc matches glibc.

## The footprint-profile contradiction, resolved

The profile cannot move this workload, and the numbers now show
why: footprint's large-cache cap is 8 MB — still above the
3.1 MB trail, so the trail is cached under both profiles — and
its eager-release knobs have nothing to release (no span is dead
during the 0.8 s run; the residues are partially live by
construction). Balanced vs footprint snapshots differ by ≤200 KB
at every phase on the current build. The round-5 paired A/B that
showed movement predates ADR 0024's regrouping build; the full
session's non-movement (19.1 vs 18.8 MB) is the behaviour that
reproduces.

## Decision

No lua-specific fix is adopted. The peak is returnable
retention, not live data and not a leak — an explicit
`malloc_trim` returns it (the bursty2 trim decomposition in ADR
0030 shows the same machinery reaching the floor) — but a
0.8-second batch process offers no ageing window, and the two
structural components (large-cache trail, class segregation)
are deliberate trades scored elsewhere. The one lever that
attacks the trail itself is in-place realloc growth (ADR 0033):
an `arr` that extends its mapping leaves no superseded blocks to
cache. Lua is scored honestly as a **peak-RSS loss to glibc and
mimalloc** in the round-6 Pareto table, with the mechanism named,
and a **time win**.
