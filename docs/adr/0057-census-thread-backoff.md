# 0057 — Per-thread census backoff

Status: Rejected for adoption (2026-11-13).

## Hypothesis and change

Repeated busy automatic attempts might waste cycles and disturb the owner.

After a failed automatic trylock, skip the next three opportunities on that thread. Reset TLS census results before every skip. Explicit trim still blocks and does not consume the backoff.

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

| case | trylock median (range), s | backoff median (range), s | delta |
|---|---|---|---|
| local | 0.461152 (0.424467–0.505730) | 0.466160 (0.432502–0.485112) | +1.09% |
| cross | 0.334765 (0.317459–0.362806) | 0.326176 (0.315563–0.401741) | -2.57% |
| many | 1.317377 (1.029309–2.165995) | 1.022327 (0.716773–1.210847) | -22.40% |
| census | 2.863166 (2.689168–3.148992) | 2.800117 (2.748821–2.956984) | -2.20% |

## Decision

Reject. Cross improves 2.57% but its ranges overlap; local regresses 1.09%. Many improves 22.40% and census improves 2.20%, also with overlapping ranges. The required non-overlapping cross-free win with no
local/many/census regression is absent. Do not promote a noisy median gain.
No claim of preserving the historical local win is made: that win was not
reproduced in ADR 0054's session. Rejected code is generated only under ignored
`work/round11`; ordinary make does not compile it.

Raw samples and summaries: `bench/results/round11-crossfree/trylock-vs-backoff/`.
See `bench/round11/README.md` for reproduction and ADR 0054 for diagnostic
limits. Correctness verification of the unchanged default is recorded in
`docs/round11-2026-11-13.md`.
