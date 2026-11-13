# 0054 — Cross-free under census trylock: reproduce and diagnose

Status: Accepted measurement; no allocator change (2026-11-13).

## Hypothesis and experiment

A busy automatic census might skip reclamation and slow cross-thread free.
Compare `FM_CENSUS_TRYLOCK=1` with `=0`, then count actual skips, reclamation
and span creation separately. Also compare two byte-identical trylock DSOs.

Based on main `e598f28`, branch `round11/crossfree-trylock`. GCC/O3,
shared Xeon Platinum 8573C, three visible CPUs and a two-CPU cgroup quota.
One excluded warmup pair, five measured alternating pairs per workload;
wall time includes subprocess startup/exit. All measured samples retained.
No correctness tests overlapped timings. Local is eight threads × 10M
operations; cross is two threads × 10M transfers (the legacy final `8`
argument does not change cross). Many/census use the unchanged ownership
harness, four pairs × 200k transfers; census adds explicit trims.
No quiet-machine, application-matrix or exhaustive race proof is claimed.

## Measurements

| case | blocking median (range), s | trylock median (range), s | delta |
|---|---|---|---|
| local | 0.466952 (0.455235–0.502850) | 0.486729 (0.439529–0.623906) | +4.24% |
| cross | 0.380748 (0.301760–0.428490) | 0.417648 (0.305336–0.530855) | +9.69% |
| many | 3.759282 (2.261536–4.314455) | 1.448886 (0.839620–1.590822) | -61.46% |
| census | 3.794581 (2.725529–4.463748) | 2.649947 (2.481448–3.023351) | -30.16% |

| diagnostic count | blocking median (range) | trylock median (range) |
|---|---|---|
| attempts | 15 (12–22) | 16 (13–25) |
| bail | 0 (0–0) | 0 (0–0) |
| walked | 24576 (5636–27910) | 26112 (16640–33291) |
| released | 196608 (0–458752) | 393216 (0–589824) |
| purged | 163840 (57344–290816) | 184320 (106496–311296) |
| spans | 29 (12–32) | 29 (20–33) |

| case | trylock median (range), s | identical median (range), s | delta |
|---|---|---|---|
| cross | 0.322594 (0.306128–0.338058) | 0.332934 (0.318261–0.427750) | +3.21% |

### Suppress census through the existing depth gate

A further five alternating measured pairs (plus one excluded warmup pair)
set `FM_RELEASE_MIN_DEPTH=65536` for both unchanged libraries. A separate
five-pair diagnostic series confirmed **zero census attempts in every run**.
Thus neither pthread census acquisition path runs in this control. This is
an attribution control, not a proposed release policy.

| case | blocking-no-census median (range), s | trylock-no-census median (range), s | delta |
|---|---|---|---|
| cross | 0.333476 (0.299584–0.503362) | 0.345160 (0.299732–0.429956) | +3.50% |

A +3.50% cross median difference remains with both census paths suppressed;
ranges overlap. Together with the identical-binary result, this shows that a
few-percent median difference can occur outside census lock execution. It
does not identify the entire original +9.69% or establish a persistent causal
API regression. No candidate repairs the measured cross-free regression to
the required non-overlapping standard.

## Decision and limits

The five diagnostic trylock runs acquired all 87 attempted censuses;
there were **zero bail-outs**, versus zero of 82 blocking attempts. They
returned 1,572,864 bytes through whole-span unmapping and 983,040 bytes through
page purge in aggregate, versus 1,114,112 and 888,832 for blocking. New-span
requests had the same median (29). These counters do not support missed
reclamation through failed trylocks as the cause in this workload/session.
Counters are atomic, diagnostic-only, and excluded from throughput results.
A forced-busy self-check verifies that the bail counter really increments,
and that explicit trim subsequently acquires the mutex.

Cross has a single sustained freeing thread, so it normally has only one
census caller; neither explicit trim nor idle release is enabled. The original
harness's bounded ring calls sched_yield when full/empty. Diagnostic wait4
measurements record variable context switches (blocking voluntary 16–179,
trylock 50–101; involuntary 15–51 and 9–39), but do not isolate scheduler,
cache layout, ring pacing, or uncontended mutex API costs from each other.
The identical-binary experiment below produces a 3.21% median difference
without any allocator change. This establishes appreciable session/queue
variability, not proof that the entire +9.69% is noise. The persistent
cross-free regression remains disclosed; an exact residual cause needs a
quieter, CPU-controlled experiment. Do not attribute it to missed reclamation
or claim it fixed.

Diagnostic peak_kib is retained as raw wait4 evidence only: fork/exec can
inherit the Python launcher's high-water mark, so it is not an allocator RSS
comparison. Released/purged counters count bytes requested, not residency.

Reproduction: `bench/round11/README.md`. Raw evidence:
`bench/results/round11-crossfree/`. ADR 0049 stays in force. ADR 0035 is
unchanged: global census mutex, atomic dead_mark, and census-before-central
fork ordering.
