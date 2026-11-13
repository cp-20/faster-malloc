# 0049 — Opt-in shortlived profile disables the large cache

Status: Accepted opt-in profile; all gates passed (2026-11-13).

## Hypothesis and change

Large JSON/string temporaries retained between phases inflate peak RSS while the
next live object graph is built. Disable large-block caching with the existing
`FM_LCACHE_MAX_BYTES=0` knob, then expose that one-setting recipe as
`FM_PROFILE=shortlived`. All other settings inherit balanced; individual knobs
still override the preset. The shared core applies equally to v2 and JIT.

A process-exit drain cannot reduce an earlier high-water mark; thread-exit already
drains its cache. Avoid new exit callbacks or pressure heuristics when a preset
can remove the measured retention. Do not change balanced defaults or add a
background worker. The cost is more mmap/munmap work for large-block churn.

## Measurements and gates

Five interleaved Python/Node samples, three diagnostic snapshots, and five
balanced/shortlived pairs of bursty idle2 and consprobe settled RSS are recorded
in `bench/results/round10-shortlived/`. The round-10 short-lived section reports
medians with ranges and limitations. `make` and the full `bench/round7.sh` battery,
including the added shortlived profile basic checks for v2 and JIT, must pass.

Adopt only the opt-in preset: peak RSS drops 12.1% (Python) and 10.4% (Node).
The gap to glibc remains 1.1% and 3.4%. Timing ranges overlap; make no speed claim.

Five runs each, median (min–max), same-session measurements:

| App/configuration | Peak RSS KiB | Seconds |
|---|---:|---:|
| python/v2 | 502,884 (502,884–502,884) | 2.927 (2.674–3.000) |
| python/shortlived | 441,824 (441,668–441,828) | 2.845 (2.817–2.997) |
| node/v2 | 498,892 (497,072–500,372) | 1.342 (1.277–1.564) |
| node/shortlived | 446,888 (445,912–450,232) | 1.339 (1.256–1.490) |

The initial profile battery failed `large-cache mapping not reused`: that
assertion deliberately requires caching, not merely allocator correctness.
`test_basic --no-large-cache` disables only that assertion and the following
spare-capacity pointer-stability assertion while preserving allocation, writes,
calloc zeroing and realloc content checks. Default and other profile invocations
keep both assertions. The full battery then passed; the initial log is retained.

Five guard runs each, median (min–max), KiB:

| Guard | Balanced | Shortlived |
|---|---:|---:|
| bursty idle2 | 42,748 (41,796–43,024) | 42,476 (41,792–42,988) |
| consprobe settled | 7,860 (7,840–7,868) | 7,860 (7,840–7,864) |

`make` and the full round-7 battery passed for v2 and JIT; the final log ends
with `epoch stalled-reader grace ok`. No settled-RSS guard regression.
