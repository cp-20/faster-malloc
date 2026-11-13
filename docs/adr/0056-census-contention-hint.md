# 0056 — Advisory census busy hint

Status: Rejected for adoption (2026-11-13).

## Hypothesis and change

Avoid the uncontended trylock API cost by using a cheap occupancy hint.

A relaxed atomic hint is set while a census owns the mutex and cleared before unlock. Automatic calls trylock when the hint is set, otherwise block. A stale hint affects scheduling only; every actual census acquires the same mutex. Fork still acquires the same mutexes in the same order.

The isolated generator in `bench/round11/build.py` retains the exact prototype.
No production source/default was changed. All span operations still execute
under the single census mutex. Central header ownership, atomic dead_mark,
and fork ordering from ADR 0035 are untouched. Hint/backoff add advisory state
only; neither state grants ownership. TLS results are reset before bailing.

## Measured results

Based on main `e598f28`, branch `round11/crossfree-trylock`. GCC/O3,
shared Xeon Platinum 8573C, three visible CPUs and a two-CPU cgroup quota.
One excluded warmup pair, five measured alternating pairs per workload;
wall time includes subprocess startup/exit. All measured samples retained.
No correctness tests overlapped timings. Local is eight threads × 10M
operations; cross is two threads × 10M transfers (the legacy final `8`
argument does not change cross). Many/census use the unchanged ownership
harness, four pairs × 200k transfers; census adds explicit trims.
No quiet-machine, application-matrix or exhaustive race proof is claimed.

Compare against a freshly measured ordinary trylock control in this series,
not a control pooled from earlier series. Each cell has five measured samples.

| case | trylock median (range), s | hint median (range), s | delta |
|---|---|---|---|
| local | 0.436552 (0.422956–0.472140) | 0.431212 (0.421442–0.449729) | -1.22% |
| cross | 0.194245 (0.191704–0.340857) | 0.328479 (0.241544–0.426227) | +69.11% |
| many | 1.249397 (1.197538–1.734792) | 1.811190 (1.050129–2.017899) | +44.97% |
| census | 3.083074 (2.747229–3.382273) | 3.009621 (2.831185–3.065002) | -2.38% |

## Decision

Reject. Cross regresses 69.11% and many regresses 44.97%; all case ranges overlap. The required non-overlapping cross-free win with no
local/many/census regression is absent. Do not promote a noisy median gain.
No claim of preserving the historical local win is made: that win was not
reproduced in ADR 0054's session. Rejected code is generated only under ignored
`work/round11`; ordinary make does not compile it.

Raw samples and summaries: `bench/results/round11-crossfree/trylock-vs-hint/`.
See `bench/round11/README.md` for reproduction and ADR 0054 for diagnostic
limits. Correctness verification of the unchanged default is recorded in
`docs/round11-2026-11-13.md`.
