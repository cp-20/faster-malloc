# 0021 — Refill-side regrouping: measured, and rejected like its flush-side parent

*Status: Rejected after measurement (2026-11-12). Recorded
contemporaneously. The code lived on the round-4 branch for one
day and was reverted; this ADR is the record.*

## Context

ADR 0014 rejected *flush-side* free-list shaping (walk the list at
publication, group by span into a stash) and named the untried
variant explicitly: "refill-side regrouping (walk a popped batch
once, on the allocating thread, instead of at flush) remains an
untried variant." Round 4 tried it, in two forms.

## Form 1: aggregate ~4,096 blocks, group, serve in group order

When a refill pops a non-pure batch and the central stack is deep
(≥ 8 batches — a mass-free event just happened), pop more batches
to ~4,096 blocks, bucket-walk them by span (hash table, with a
last-span cache for clustered runs), and adopt the grouped list.

Result: the `locality` metric did not move at all (**512.5 pages**
vs baseline 512.9). The trace showed regrouping firing correctly
every burst — 4,096 blocks over 33 spans — and the burst then
consumed *the entire haul*, touching every span anyway. Grouping
a sample of the pool cannot concentrate a burst that eats the
whole sample.

## Form 2: group the whole stack, serve largest groups first

Aggregate the entire central stack (bounded at 32,768 blocks),
group by span, sort groups by size descending, adopt the largest
~4,096 blocks locally, and push the remaining groups back as
span-pure batches with known counts (the form the ADR 0015/0020
census accounts wholesale; a group tail termination bug in the
first cut — pushed-back groups kept stale links — was found by
the stress battery and fixed before measuring).

The metric now does exactly what ADR 0014's stash did:

| metric | baseline | form 2 |
|---|---|---|
| locality pages/burst | 512.9 | **67.7** (floor: 64; ADR 0014 stash: 67.8) |

And the tax is the same family as ADR 0014's, reduced but not
avoided (min-of-5 micro, taskset; a compiled-out control build
with only the depth gate raised confirms the delta is the
regrouping itself):

| workload | control (no regroup) | form 2 | 
|---|---|---|
| batch 64 B | 8.64 ns/op | **12.30 ns/op** |
| hold 64 B | 11.40 ns/op | **14.85 ns/op** |
| churn 64 B | 6.91 ns/op | 7.33–7.85 ns/op |
| threads | 7.75 ns/op | 8.11–8.64 ns/op |
| locality time | 7.3 ns/op | 33–38 ns/op |

## Why the tax is structural

The walk is a serial pointer chase over cold blocks: ~22 ns per
block on this VM, and no prefetching fixes a dependency chain.
Form 2 must walk the *whole pool* to earn its metric (form 1
proves a partial walk earns nothing), so any workload whose cycle
is "free a pool, reallocate it" pays ~22 ns × pool per cycle —
batch/hold pay ~35 % — while the benefit only exists when the
reallocation burst is *much smaller* than the pool and followed
by enough compute on the concentrated pages to matter, a ratio
the timed suites do not contain. There is also no gate that
separates the cases: batch's central depth at the refill (~390
batches) is *higher* than locality's (~128), and the span cache
that should have made clustered walks cheap does not help,
because the chase latency, not the hashing, is the cost.

## Decision

**Rejected**, under the same standard ADR 0014 set: the mechanism
works and the metric proves it, but the tax lands on the timed
workloads and the refund does not. Kept from the experiment:
nothing in the allocator (the `fm_central_pop` refactor went with
the revert), and two facts for whoever revisits this — grouping
must be whole-pool and largest-first to move the metric at all,
and on hardware where a cold chase is cheaper relative to the
compute a concentrated burst feeds, the trade should be repriced
with the `locality` scenario, which remains the instrument.
