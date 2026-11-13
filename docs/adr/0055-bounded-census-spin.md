# 0055 — Bounded census spin

Status: Rejected for adoption (2026-11-13).

## Hypothesis and change

A few retries might avoid transient skips without forming a blocking convoy.

After the initial failed trylock, retry at most 16 times with x86 pause. Explicit trim still blocks.

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

| case | trylock median (range), s | spin median (range), s | delta |
|---|---|---|---|
| local | 0.439737 (0.432087–0.475128) | 0.430034 (0.416731–0.454835) | -2.21% |
| cross | 0.307969 (0.203159–0.437281) | 0.412533 (0.191594–0.568301) | +33.95% |
| many | 1.193907 (0.916869–1.380614) | 1.272753 (1.065417–1.745073) | +6.60% |
| census | 2.851155 (2.681311–4.286773) | 2.708919 (2.405347–3.146549) | -4.99% |

## Decision

Reject. Cross regresses 33.95% and many regresses 6.60%; all case ranges overlap. The required non-overlapping cross-free win with no
local/many/census regression is absent. Do not promote a noisy median gain.
No claim of preserving the historical local win is made: that win was not
reproduced in ADR 0054's session. Rejected code is generated only under ignored
`work/round11`; ordinary make does not compile it.

Raw samples and summaries: `bench/results/round11-crossfree/trylock-vs-spin/`.
See `bench/round11/README.md` for reproduction and ADR 0054 for diagnostic
limits. Correctness verification of the unchanged default is recorded in
`docs/round11-2026-11-13.md`.
