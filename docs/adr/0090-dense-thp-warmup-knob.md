# 0090 — Dense-extent THP warmup is a runtime knob

Accepted 2026-11-25.

## Context

ADR 0089's experiment removed the `MADV_HUGEPAGE` hint on the small-class
dense extent and cut git by 1.9 MiB and lua by 2.0 MiB, but the paired
11-rep matrix measured jq at +7.9% time (the hint carries jq's TLB
coverage).  The warmup threshold cannot separate them: with 4 extents
(8 MiB) jq, git and lua all receive the hint; with 8 (16 MiB) all three
lose it, because jq's dense demand (8-16 MiB) and git/lua's (8-10 MiB)
overlap.

## Decision

Keep the hint on by default (warmup 4 extents) and expose the threshold
as `FM_DENSE_THP_WARMUP` (0 disables, up to 4096 extents).  A
memory-first deployment sets it to 0 and buys git -1.9 MiB / lua -2.0 MiB
at jq's +7.9% time; the default keeps jq's time.

## Evidence

| `FM_DENSE_THP_WARMUP` | git RSS | jq RSS | jq time |
|---|---:|---:|---:|
| 4 (default) | 23.9 MiB | 268.8 MiB | baseline |
| 8 | 22.0 | 258.2 | - |
| 0 | 22.0 | 257.1 | +7.9% (paired 11-rep) |

`test_basic`, `stress`, `rtest` and `mtrepeat` pass with the knob.
