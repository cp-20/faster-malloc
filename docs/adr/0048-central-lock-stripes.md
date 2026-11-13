# 0048 — Stripe central-stack ownership by class group

Status: Rejected for default adoption (2026-11-13).

## Hypothesis and change

Sixteen cache-separated central mutexes selected by class modulo 16 could
reduce unrelated classes contending on one lock. Census ownership stays global.
Push/pop/detach for a class take its stripe before reading any shared header
link and release before walking/recycling. Fork takes census then all stripes
in ascending order, releasing in reverse. Header recycling remains private
past removal, so no stale reader survives reuse. No epoch redesign is attempted.
The prototype was compile-time `FM_CENTRAL_STRIPES`, default off. Its exact
source diff is retained in `bench/round10/central-stripes.patch`, applied to
f90550c by the reproduction script; it is absent from shipped allocator code.

## Measurement

Same-container f90550c O3 v2 control; one warmup pair, five alternating measured
pairs, original round-8 workloads and budgets. Medians and full ranges in seconds:

| case | base median (range), s | stripe median (range), s | delta |
|---|---|---|---|
| local | 0.415081 (0.402475–0.486059) | 0.413491 (0.396533–0.572848) | -0.38% |
| cross | 0.307681 (0.287335–0.373814) | 0.296989 (0.292092–0.398598) | -3.48% |
| many | 3.364578 (2.140001–3.779003) | 3.130781 (2.166995–3.441062) | -6.95% |
| census | 3.316879 (3.024515–3.497088) | 3.201610 (2.639468–3.267405) | -3.48% |

Faster pairs: local 3/5, cross 3/5, many 3/5, census 5/5. All endpoint/count
checks passed. Ranges overlap in every cell; local barely moves. Reject the
additional fork/lock machinery for this modest, noisy evidence. This does not
establish that striping never helps. No default promotion or exhaustive
correctness validation is claimed for this rejected patch. Linux/glibc's
zero-initialized normal mutex representation was used by this Linux prototype;
a portable adopted implementation would need explicit mutex initializers.

Raw samples, CVs, host details and reproduction commands are under
`bench/results/round10-lock-cost/` and `bench/round10/lock-cost.sh`.
Two brief candidate/control compilations occurred during this first series;
no sample is excluded. No correctness battery overlapped timing. Shared host,
two-CPU quota: indicative measurements, not reference-machine claims.
