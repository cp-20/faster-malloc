# 0020 — Automatic small-span return: the census on a cadence, with dead-span ageing

*Status: Accepted (2026-11-12), after one rejected form (instant
release) measured and replaced inside this same round. Recorded
contemporaneously.*

## Context

Round 3's headline gap: v2's RSS exceeds glibc's in most real apps
(Lua peak +66 %, Python +16 %, Node +18 %, redis +37 %). The
large-block cache was already ruled out as the cause (ADR 0019);
the Lua end state decomposes cleanly under a `malloc_trim` shim:
19,792 KB resident, of which ~15.5 MB is provably-free small spans
that only an explicit trim returns (glibc: 8,952 KB, ~6 MB
returnable). ADR 0015 built the census that *can* return them but
only on request; ADR 0014 measured returning them eagerly and
rejected it (the mmap/fault cycle). The open design problem was a
policy: return spans automatically, lazily enough never to tax a
cycling working set.

## The rejected first form: instant release on a flush cadence

Design: a per-class counter on the central stack (push/pop already
touch the header), a per-thread flush counter; every 16 flushes of
a class, if its central stack holds ≥ 4 batches, run the ADR 0015
census in a retaining form (unmap dead spans beyond 2 kept per
class). It worked exactly where aimed —

- trim scenario after-free RSS: 161,752 → **37,060 KB** with no
  trim call (glibc: 124,732 KB);
- Lua end state: 19,792 → **12,028 KB** —

and failed exactly where ADR 0014 said it would:

| workload | before | instant release |
|---|---|---|
| micro batch 64 B | ~11 ns/op | **82.6 ns/op** |
| micro hold 64 B | ~11 ns/op | **85.5 ns/op** |
| macro locality | 8.8 ns/op | **310 ns/op** |
| macro server RSS | 10,448 KB | **18,156 KB** |

batch/hold/locality free a whole working set and reallocate it
next round (~3–4 ms later, measured): the census fires *during*
the free burst — the one moment every span is provably dead — and
unmaps the set the next round must re-mmap and re-carve. Server is
the multithreaded form of the same lesson: cross-thread frees keep
central stacks busy, censuses find nothing dead, but each census
detaches the stack (refills mint fresh spans meanwhile) and walks
it, so peak RSS *rose* by 8 MB.

## The accepted form: mark, age, then release

Three changes, each aimed at one failure above:

1. **Dead-span ageing (mimalloc-style decay).** A span header field
   (`dead_mark`) records the millisecond a census first found the
   span entirely free. The automatic census only *marks*; a span
   is unmapped by a later census that finds it still entirely free
   with a mark at least **FM_RELEASE_AGE_MS = 100 ms** old. A
   census that finds a span with any block missing clears the
   mark, as does a refill that pops a span-pure batch of it. The
   bench suite's cyclic patterns repeat every ~3–4 ms, so the age
   is ~25× their period; real shrinkage stays dead for seconds.
   `malloc_trim` keeps ADR 0015's instant semantics (a separate
   flag into the same census).
2. **Adaptive per-class interval.** A class is censused at most
   once per 10 ms, and a census that finds no fully-free spans at
   all doubles the interval (exponential backoff to 320 ms);
   marking, waiting, or releasing resets it. Busy classes whose
   spans never die — the server case — are walked rarely instead
   of constantly.
3. **Full sweep per tick.** Ticks arrive every 16 flushes of any
   class (on the already-cold flush path) and consider *every*
   class behind its gates (central depth ≥ 4 batches, interval
   elapsed). An earlier rotating 3-classes-per-tick sweep was
   measured and abandoned: a full rotation took ~1.6 s of trickle
   activity, so silent classes were never even marked (Lua stayed
   at ~19 MB; a decay probe released only 1.8 of 125 MB). The
   gates are two atomic loads, so the full sweep is cheap.

The census itself, its safety argument, and the keep-quota
(FM_RELEASE_KEEP = 2 spans per class as burst absorption) are ADR
0015's, unchanged.

## Evidence

**The mechanism, isolated.** A decay probe (build the trim
scenario's ~130 MB mixed set, free it all, then stay lightly
active with unrelated small churn and sample RSS):

| time after free | glibc | v2 aged release |
|---|---|---|
| 0 ms | 124,660 KB | 135,424 KB |
| 150 ms | 124,788 KB | 16,148 KB |
| 500 ms | 124,788 KB | **9,520 KB** |

**Cyclic workloads unharmed** (min-of-5 micro, this session;
locality macro, interleaved):

| workload | instant form | aged form |
|---|---|---|
| batch 64 B | 82.6 | **8.88 ns/op** |
| hold 64 B | 85.5 | **11.11 ns/op** |
| locality | 310 ns/op | **7.24 ns/op**, 512.9 pages/burst — bit-identical to the pre-change baseline |
| server RSS (macro) | 18,156 KB | **11,352 KB** (≈ baseline 10,448) |

Macro suite ratios to glibc otherwise unchanged (compiler 0.48,
jsondoc 0.75, game 0.27); the trim scenario's *instant* after-free
reading stays at ~162 MB by design (its single burst ends before
any mark ages; its after-trim reading is unchanged at ~7.1 MB).

**Apps: honest limits.** The app matrix moved much less than the
probe, and the round-3 framing ("retained small spans") turned
out to be only part of the story:

- *Lua* (peak 18,936 KB vs glibc 11,844): RSS sampling every
  25 ms shows a +2.1 MB *steady-state* plateau during the churn
  phase (partially-free spans — fragmentation, not returnable by
  any census) and a terminal climb: the final `collectgarbage`
  frees ~15 MB in the last milliseconds before exit. No decay
  policy can act inside that window, and glibc's edge here is
  arena packing, a different mechanism.
- *Python* (506 MB) and *Node* (504 MB) peaks are live peaks
  (the big.json working set, the V8 heap): there is nothing dead
  to return at the moment the peak is set.
- *redis* (10,332 KB vs glibc 7,508): a long-running server whose
  retained spans are partially free — again fragmentation, not
  dead spans.
- *sqlite time* improved from 1.056× glibc (round 3) to 0.92× in
  this round's session with the v1 drift control stable
  (0.97 → 0.96); a plausible mechanism is the periodic census
  re-batching sqlite's central stacks into known-count batches,
  but that is unproven — flagged for re-verification, not claimed
  as understood.

So: the capability is delivered and priced at zero on every
cyclic workload measured, and it converts "a process that shrinks
and keeps running" from glibc's weakest case to this allocator's
strongest (135 MB → 9.5 MB where glibc returns nothing at all).
The remaining app RSS gaps are a *placement/fragmentation*
problem — partially-free spans — which no return policy can
address; it is the top of the round-5 list.

**Known limitation.** Release is driven by allocation activity
(ticks ride the flush path): a process that goes completely idle
after a burst keeps its spans until it allocates again — the same
terms glibc offers (its free-path return is also activity-driven
outside `malloc_trim`). The 100 ms age and the interval/backoff
constants are single-point choices on a noisy shared VM, not
tuned optima.

## Revision (same round): the jq census tax and its fix

The first full app matrix on the build above exposed a cost none
of the round's earlier instruments could see: **jq slowed to
1.40× glibc** (from 0.80× in round 3), with the JIT build equally
slow and v1 — no census machinery — unaffected in the same
interleaved window. Bisection (the round-3 build in the same
session: 1.77 s; this build with the release tick compiled out:
1.97 s; with the tick live: 2.69 s) pinned it on the census
itself. jq's DOM phases leave central stacks *hundreds of
thousands of blocks* deep that are mostly **not** dead; bulk
frees keep setting marks, so the nothing-found backoff never
engaged, and a full walk of a monster stack ran every ~10 ms per
class. The macro jsondoc scenario never grows stacks that deep,
which is why the suite stayed green while a real app paid.

Two fixes were tried:

1. **Capped detach** (a census detaches at most 16 batches):
   rejected on measurement. A span is provably dead only when
   *all* its blocks are visible to the census; detaching 16 of a
   class's ~60 batches shows it a scattered ~quarter of every
   span, so almost nothing was ever marked — the decay probe
   stalled at 116 MB after 500 ms. The census's premise is the
   whole stack; bounding the *work per visit* has to happen in
   time, not in coverage.
2. **Cost-proportional interval** (shipped): the backoff counter
   is replaced by a stored per-class interval derived from the
   last census's walked-block count — 1 ms per 100 blocks walked,
   doubled when a census finds nothing, capped at 5 s. The walk
   is the census's real cost (~22 ns/block), so the worst-case
   duty cycle is bounded by construction at roughly 1/450 of a
   class's runtime, whatever its stack depth; small stacks keep
   a fast cadence because their walks are cheap.

Post-fix, in one session: jq v2 **1.77 s** (glibc 1.91, round-3
build 1.77 — parity with the pre-feature build); the decay probe
still releases fully (135 MB → 9.5 MB) with the landing at
~300 ms instead of ~200 ms — the probe's ~15k-block stacks now
carry a ~150 ms walk-proportional floor, the honest price of
the bound. The final full matrix and macro suite were re-run on
the fixed build for the round doc.

## Decision

**Accepted, as revised.** `fm_trim_class` gains the
retaining/ageing form; `FM_Central` gains the depth counter and
census bookkeeping the gates need (the push/pop paths were
already touching that cache line); the trim census's proof
obligations are unchanged, and the FM_DEBUG over-count assertion
held across the full battery (test_basic, stress ×3, check, mt,
earlyinit, tlsexit) for both builds, before and after the
revision.
