# 0082 — 4 MiB per-thread large slab

Accepted 2026-11-24.

## Context

`fm_map_large_slab()` gives each thread one 2 MiB slab for >4 KiB class
spans and large allocations up to 512 KiB.  When the next request does not
fit, the remainder is unmapped and a fresh slab is mapped.  g++/cc1plus
cycles 64 KiB obstack chunks, and jq/xz have medium-buffer phases, so the
slab boundary is crossed often.  The slab geometry had not been swept
since ADR 0077.

## Decision

Map 4 MiB + one span instead of 2 MiB + one span.  Everything else is
unchanged: per-thread TLS cursor, 64 KiB carve alignment, `raw_out` /
`len_out` sub-range accounting, drain on `malloc_trim` and thread exit,
fork inheritance, and the >512 KiB large path.

## Evidence

`exp/large-slab`, 3 repetitions, CI runner:

| case | ratio |
|---|---:|
| `python_data` | 0.980 |
| `gxx_compile` | 0.983 |
| `sort_p4` | 0.983 |
| `jq_large` | 0.998 |
| `sqlite_oltp` | 0.998 |
| real-app geomean | **0.9901** |
| micro geomean (Ir) | 0.9999 |

Peak RSS was flat in that run.  Untouched tail pages remain non-resident.

## Risks

* Up to ~4 MiB of unused tail VA per thread stays mapped until trim/exit
  (virtual only).
* A 4 MiB anonymous mapping is more likely to be collapsed under a
  system-wide `always` THP policy than a 2 MiB one; this code does not
  issue `MADV_HUGEPAGE` for the slab.
* If a workload consumes slabs exactly, this only halves the refill rate
  and may measure flat; the CI result is the gate.
