# 0048 — Avoid waiting behind another census on automatic release

Status: Accepted as the measured default (2026-11-13).

## Hypothesis and change

Automatic census opportunities need not queue behind an active census.
Using trylock for non-instant censuses avoids an allocator-thread convoy;
explicit malloc_trim still takes the blocking census mutex. Prototype:
`FM_CENSUS_TRYLOCK`, default off during measurement, now default 1.
Compile with `-DFM_CENSUS_TRYLOCK=0` to retain the blocking control. No central lock, epoch,
batch size, purge geometry, cadence constant or profile is changed.

Reset thread-local census result counters before acquiring ownership, so a
busy return cannot reuse the previous census's accounting. On trylock failure,
return zero before flushing a TLS bin, detaching central batches, allocating
scratch, or reading/writing span metadata. The caller's existing adaptive
interval bookkeeping observes an empty result and schedules a later opportunity.
This is deliberately a skipped opportunity, not a delayed detached census.

## ADR 0035 ownership argument

Every actual census still holds the single census mutex across all span
purged-mask/withheld-count/age operations. Central push/pop/detach retain the
original mutex, including link reads before header ownership/recycling. Census
result counters remain TLS; dead_mark remains atomic. Intersect/start counts
and withheld geometry are untouched. A failed trylock owns no blocks and touches
no shared span state. Fork still takes census then central, and parent/child
release in reverse; no additional lock or reader lifetime is introduced.
Thread-exit flushes follow the unchanged central protocol. No stalled reader
is declared dead or batch guessed free.

Both allocation-driven automatic release and the opt-in idle worker may skip
a busy class census. Reclamation can therefore be delayed under sustained
contention; there is no bounded automatic reclamation guarantee. Explicit trim
still waits, and later automatic ticks/idle sweeps may retry. This changes
background reclamation scheduling, not the definition of a reclaimable page.

## Measurement and decision

Exact f90550c machine-code control, same container/compiler/O3 v2. One excluded
warmup pair plus five measured alternating pairs per case. No best-of-N,
no discarded samples, no builds/tests overlapping this adaptive series.
Original round-8 operation counts and ownership harness are unchanged.
Medians and full ranges in seconds:

| case | base median (range), s | adaptive median (range), s | delta |
|---|---|---|---|
| local | 0.428888 (0.419202–0.439033) | 0.403827 (0.390341–0.411724) | -5.84% |
| cross | 0.299039 (0.294726–0.319081) | 0.310259 (0.275807–0.360175) | +3.75% |
| many | 3.992888 (2.788290–4.300111) | 1.178907 (0.922673–1.713293) | -70.47% |
| census | 3.238726 (2.765242–3.273242) | 2.524982 (2.360714–2.812997) | -22.04% |

Faster pairs: local 5/5, cross 2/5, many 5/5, census 5/5. Local and many ranges
are separated. Census ranges overlap slightly; all five pairs improve.
Cross is noisy and its median regresses 3.75%; do not claim a universal speedup
or a pure per-lock cost reduction. The material many/census gains support
adoption after the full suite passed. This avoids redundant queued work rather
than weakening ownership. Shared two-CPU-quota cloud hardware is not a quiet
reference machine. Raw outputs, samples, CVs and host metadata are retained.

## Validation

Both pre-promotion default and adaptive `make -B -j4` / full `bench/round7.sh`
batteries passed, including v2/JIT, profiles, twelve-thread purge/churn stress,
realloc-cache/idle opt-ins, fork, early-init, TLS exit, lifecycle fault injection
and the retained epoch grace test. Separate FM_DEBUG static/JIT builds for both
configurations passed three stress repetitions each (12 runs), with age zero,
minimum one purge page, idle release and realloc-cache search enabled.
The focused busy-lock regression passes: TLS lists/central head unchanged,
TLS result counters reset, no census recursion, and a subsequent explicit trim
works. Its initial compilation needed a missing stdio include; no runtime
failure occurred. Transcripts are retained under the results directory.

The promoted ordinary v2 build's .text matches the measured adaptive library
exactly. The final full ordinary-default round7 run passed and is recorded separately as
`promoted-battery.txt`. These checks and the ownership argument are not an
exhaustive race/sanitizer proof or controlled-hardware/application validation.
