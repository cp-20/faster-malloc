# 0024 — Occupancy-aware placement: the census rebuild regroups survivors by span

*Status: Accepted (2026-11-13). Contemporaneous.*

## Context

ADR 0020's automatic release can only unmap a span that is
*entirely* free. Round 4's app matrix showed what that leaves
behind: partially-free spans. Lua's RSS sat ~+2 MB over its live
working set in steady state, redis retained +37 % over glibc, and
most apps stayed above glibc's RSS — because nothing in the
allocator cared *which* span a recycled block came from. Refill
popped whatever mixed batch sat on top of the central stack, so
recycled blocks sprayed across every span of the class and each
span kept at least a few live blocks forever.

Round 4 rejected two refill-path attempts to fix placement
(ADR 0021 regrouping: locality improved, batch/hold +35 %;
ADR 0022 jump table). Both paid a per-operation tax on the hot
path. The question for round 5 was whether placement could be
biased from a path that already visits every block: the census.

## The probe that isolates it

`bench/consprobe` (new this round, now a standing instrument):
192 spans of the 64 B class are filled, all but every 16th block
freed (sparse survivors in every span), then a 48-span live
window churns through the central stack in bursts, and finally
the survivors are freed while the window keeps turning. Whatever
still holds a window block stays mapped; spans the window has
vacated can be reaped. Baseline v2 (round-4 build) settles at
**17.6–17.8 MB** RSS for 3.0 MB of live data — it releases
nothing in this pattern, because each refill re-sprinkles the
window over the spans the survivors just vacated. (glibc settles
at 19.2 MB, v1 at 16.2 MB: nobody returns this memory.)

## Design

Placement is decided where free blocks change hands: the central
stack. The census (ADR 0020) already detaches the whole stack and
visits every block — pure batches counted wholesale, mixed
batches walked. The rebuild phase now uses that visit to
**regroup**:

- Each span's surviving blocks are linked into one chain on its
  census entry (span-pure batches splice wholesale; mixed-batch
  blocks are appended during the walk that already happens — no
  extra pass over memory).
- The stack is re-published as one span-pure batch per span
  (fragments under `FM_REGROUP_MIN = 8` blocks merge into a single
  mixed residue batch), pushed **largest free-count first**. A
  refill therefore pops the batch of the *fullest* span — the one
  least likely to empty — and the freest spans' blocks rest at the
  bottom of the stack, untouched, so those spans can still empty
  completely as their last live blocks die and be reaped.
- The residue (fragments of the fullest spans) is pushed last,
  i.e. on top: consuming a handful of blocks from near-full spans
  is the cheapest spray there is.
- Spans whose census entry is missing (an add dropped when the
  census table could not grow) keep their blocks in the residue:
  never lose blocks, never invent counts.

What this deliberately is not: a per-allocation policy. Nothing
on the malloc/free hot path changes; the bias is refreshed at
each census, at the census's gated, cost-proportional rate.

## Alternatives considered

- *Fullest-span-first at refill time* (walk the stack per
  refill): O(depth) per refill — the shape of ADR 0021's tax.
- *Per-span free lists in central* (a structural redesign of the
  central stack): larger than a round, and the batch design's
  O(1) splice is worth keeping.
- *Segregated local lists by span* (thread caches remember span
  ids): helps only single-threaded circulation; cross-thread
  returns still land mixed in central.

## Evidence

Consprobe (interleaved, 2 runs each): baseline **17,816 /
17,640 KB** settled RSS → regrouped **9,072 / 9,032 KB**. The
window consolidated into ~its own 48 spans and ~136 spans were
returned. The decay probe (ADR 0020's scenario) is unchanged:
9.5 MB landing within ~300 ms, as before.

Speed, interleaved min-of-5 micro A/B (baseline → regrouped,
ns/op): churn 64 B 10.80 → 10.58, batch 12.38 → 11.87, hold
17.04 → 17.19, threads 11.50 → 11.32, cross 1107 → 1104 (pinned
comparison). Macro suite ratios to glibc within session are
unchanged (server 0.84 → 0.68, compiler 0.51 → 0.49, jsondoc
0.79 → 0.81, kvstore 0.88 → 0.86, game 0.35 → 0.33, locality
0.43 → 0.44) — movements track the v1 drift control; no tax is
visible anywhere, and none was expected: the regrouping cost is
paid inside a census whose interval is proportional to walked
blocks, and post-regroup censuses walk *less* (pure batches are
counted wholesale).

Apps (lua/redis/jq, apps.sh A/B in one session pair; the box was
shared with an unrelated worker this session, so read deltas,
not absolutes):

| app | round-4 build | regrouped | reading |
|---|---|---|---|
| lua | 0.869 s / 18,600 KB | 0.902 s / 19,544 KB | time within noise; peak RSS **up** ~0.5 MB |
| redis | 11.59 s / 10,304 KB | 10.79 s / 10,340 KB | time better (contention-noisy), RSS flat |
| jq | 2.861 s / 390,696 KB | 2.792 s / 390,208 KB | flat; the deep-stack guard holds |

The lua regression deserves its own paragraph, because it is
real, small, and not fully explained. Follow-up sampling
(min-of-5 peak RSS): round-4 build 18,916 KB, regrouped 19,080 KB
— the distributions overlap (both span ~±0.3 MB), so the effect
is +0.2–0.5 MB, ~2 %. Controls: with the census disabled entirely
(ADR 0027's `FM_RELEASE_MIN_DEPTH`), lua rises further to
19.6 MB, so the regrouping itself is not the cost — it sits
between no-release and release-only; and under the footprint
profile (ADR 0027) lua drops to 18,756 KB, below both. The
working hypothesis is a peak-shape interaction at lua's terminal
allocation burst (refill order vs which spans the burst faults
in); a `FM_REGROUP_MIN` sweep (8/64/256) does not move it, and
consolidation is insensitive to that knob. It is recorded here
as a known small cost, with span-level instrumentation at the
peak as a round-6 item — not silently absorbed.

Redis is flat for the reason the round-4 analysis predicted: its
retained spans hold long-lived keys — the *live set itself* is
spread across spans, which no placement policy can fix. jq's
deep stacks stay healthy: the census feedback loop the design
worried about (post-regroup censuses walk less → intervals
shrink → censuses multiply) does not materialise, because the
depth gate and the backoff on empty censuses still bind; see
ADR 0026's sweep for the interval constants under load.

Full correctness battery green, including FM_DEBUG builds (a new
assertion checks regrouped counts against census counts whenever
no census add was dropped).

## Decision

**Accepted.** The retained-free term of the footprint equation
was the allocator's biggest measured RSS liability after
ADR 0020, and this closes most of the *fixable* part: −49 % on
the probe that isolates it, neutral on redis/jq, a small
(+~2 % peak) unresolved cost on lua that the footprint profile
more than reverses. What remains is live-set spread (an
application's own doing) and the idle-process case (ADR 0029).
