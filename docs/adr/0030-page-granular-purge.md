# 0030 — Page-granular purge inside partially-free spans

*Status: Accepted (2026-11-13). Contemporaneous. Round 6, item 1.*

## The gap

Round 5's bursty cell isolated the allocator's worst footprint
loss: after a burst drains, v2's idle RSS was 88.2 MB against
mimalloc's 53.6 MB for a ~17 MB live set. v2 released memory
only in whole 64 KiB spans (ADR 0020); a span with one live block
kept all sixteen of its pages resident. mimalloc returns memory
at page granularity. The round-5 §8 backlog made closing this
the headline item of round 6.

## Decision

The census (ADR 0020/0024 machinery) learns a second form of
return: **purge** — `madvise(MADV_DONTNEED)` on individual 4 KiB
pages inside spans that still have live blocks.

A page is purgeable when two exact counts, taken over the blocks
the census holds in central, agree:

- **Intersect count**: every block intersecting the page is free
  in central (`page_free == page_tot`). No live block's bytes are
  ever discarded. Blocks the census cannot see (in some thread's
  local list) only make the count smaller — the safe direction.
- **Start count**: every block *starting* in the page is free in
  central (`page_sfree == page_stot`). A free block's link word
  lives in its first bytes, so a block that starts in a purged
  page must be one the census itself seized; a same-class block
  sitting in another thread's local list would otherwise be
  handed out later with its link word zeroed by the purge. This
  count is what makes the purge safe under concurrency, not just
  in a quiescent process.

Blocks merely straddling *into* a purged page from a kept one
are harmless: their link words stay readable, and the discarded
tail bytes belong to a free block, whose contents mean nothing.

Blocks starting in purged pages are **withheld** from
circulation: the census's rebuild drops them from every chain,
so they are in no free list and can never be handed out or freed
again while withheld. The withheld set is exactly "blocks
starting in the span's purged pages" — a pure function of the
span's `purged_mask`, recomputable at any later census with no
per-block state. That is what makes **reclaim** safe: when a
purged span's circulating supply reaches zero and its purge has
aged (`release_age_ms` since the purge landed), the census
re-carves the withheld blocks arithmetically and returns them to
circulation, re-faulting their pages on demand.

Mechanics around the decision:

- Page 0 is never purged (it holds the span header). Only
  resident pages are purged (`mincore`): an untouched page has
  no RSS to return. A span must yield at least
  `FM_PURGE_MIN_PAGES` new pages (default 4; footprint profile 2)
  — in total, with no contiguity requirement.
- Decisions age exactly like dead-span marks (ADR 0020): the
  first census that finds a span purge-worthy only stamps
  `purge_mark`; a later census past the age applies.
  `malloc_trim` purges instantly.
- The dead-span proof stays exact: a span's effective free count
  is central blocks + `withheld_cnt`, so a span whose live blocks
  all free up still unmaps whole.
- Knobs: `FM_PAGE_PURGE` (default 1; the throughput profile sets
  0), `FM_PURGE_MIN_PAGES`. JIT inherits everything via fm_core.h.

## The design error the measurements caught

The first implementation required purged pages to form runs in
which every intersecting block was *wholly contained* — the
intuition being that a circulating block must never touch a
discarded page. In the real bursty geometry it captured ~0.4 MB
of a 43 MB ceiling (bursty2 decomposition, below). The span
dumps showed why: with a 64-byte header offset and block sizes
that do not divide 4096, essentially every page boundary has a
straddling block, so the edge-shrink rule ate every run page by
page. Containment was also simply the wrong invariant — it
confused "the block's bytes are discarded" (harmless for a free
block) with "the block's link word is discarded" (fatal). The
start-count rule above is the correct pair of conditions, is
simpler (no runs), and captures nearly the whole ceiling.

## Two census-integration bugs found by the cells

1. **Purge findings held the census interval at its floor.**
   Purge candidacy is common under churn, and "a purge is
   waiting" originally counted like "a dead span is waiting" in
   the interval-doubling exemption. Bursty's drain then ran
   censuses back-to-back while the coverage pass tripled the
   walk: the drain burned ~13–17 s of CPU and did not finish.
   Purge marks/waits now have their own counter
   (`fm_census_pg_waiting`) that does not exempt the interval
   from doubling; only dead-span progress and reclaim (both
   supply signals) do.
2. **Coverage walks were charged to the cadence.** The coverage
   pass walks candidate spans' blocks a second time; charging
   that to `fm_census_walked` doubled the interval after every
   large census and starved the dead-span machinery the purge
   shares the census with: consprobe ran 2 censuses instead of 9
   and settled at 17.7 MB instead of 9.0. The walked counter now
   prices only the pre-existing counting/rebuild work (restored
   around the coverage pass); coverage cost is bounded by the
   candidate mass it can return. Consprobe settled back to
   8.9–9.0 MB, decayprobe unchanged (9.6 MB).

## Measurements (this VM, within-session)

Bursty cell (canonical harness; the final build's run, with the
round-5 figures alongside):

| allocator | idle-1 RSS | idle-2 RSS |
|---|---|---|
| glibc | 75.0 MB | 106.3 MB |
| v2 (round 5) | ~78 MB | 88.2 MB |
| **v2 + purge** | 77.8 MB | **44.2 MB** |
| mimalloc | 79.3 MB | 54.8 MB |
| jemalloc | 81.5 MB | 114.5 MB |

The headline gap is not just closed but reversed: v2 now holds
the bursty idle-2 title. Idle-2 is the stable column (v2 44–47 MB
across every run this round); idle-1 is load-sensitive (a
quieter mid-round run showed v2 at 52.4 MB). Checksums identical
across allocators.

Decomposition instrument (`bench/apps/bursty2.c`, a diagnostic
fork of bursty that computes the exact purge ceiling from live
addresses at idle-2): the ceiling is ~44 MB of fully-free pages
inside partial spans; trim with purge reaches 43.6 MB RSS vs
86.2 MB without — the mechanism captures what the geometry
offers.

Correctness: full battery green (test_basic incl. all
FM_PROFILE variants, stress ×3, check, mt both modes,
earlyinit, tlsexit), and a fresh FM_DEBUG build passes
test_basic, stress and bursty2 with all census asserts silent
(skip-count == withheld, reclaim-carved == withheld, regroup
totals). The re-warm risk (page faults when purged pages are
re-touched) is priced by the ageing: nothing is purged until it
has been free across two censuses ≥100 ms apart, and reclaim
only re-faults pages whose span is demonstrably in demand again;
the app matrix shows no time regression attributable to it
(round-6 log).

## Consequences

- Partial spans now shrink toward their live set continuously,
  not only at whole-span granularity. Span count can grow (a
  purged span stays mapped), which the consprobe result shows is
  the right trade when the cadence accounting is honest.
- The census entry grew per-page start counters; census cost on
  candidate-heavy classes rises (coverage walk), bounded by the
  cadence fix above.
