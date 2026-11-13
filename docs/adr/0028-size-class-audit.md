# 0028 — Size-class audit: the table is already well-placed; waste lives elsewhere

*Status: Accepted (2026-11-13). Contemporaneous.*

## Context

Memory efficiency has two independent terms: **internal**
fragmentation (a request rounded up to its size class) and
**retained-free** memory (freed blocks the allocator keeps
mapped). ADR 0024 attacks the second; this audit measures the
first, per allocator, empirically: `bench/usableprobe` (new,
standing) prints `malloc_usable_size` for every request 1..8192
(and sampled to 1 MB) under each LD_PRELOAD, so the comparison
uses each allocator's *actual* table on this system, headers
included — not documentation. (jemalloc and mimalloc's tables
turn out nearly identical in these ranges; their aggregate rows
coincide to four decimals, which the raw CSVs confirm is a real
table similarity, not a preload mix-up — the files differ.)

## Measured internal fragmentation

Average usable/requested over uniform request sizes in range:

| allocator | 1–512 B | 513–8192 B | avg waste 513–8192 |
|---|---|---|---|
| glibc | 1.176 | **1.003** | 7.5 B |
| **v2** | **1.127** | 1.022 | 90.2 B |
| jemalloc | 1.130 | 1.089 | 362.2 B |
| mimalloc | 1.130 | 1.089 | 362.2 B |
| tcmalloc | 1.096 | 1.076 | 348.3 B |

Worst single step: v2's is 4352/4097 = 1.062 (at the 4 KB class
boundary); jemalloc/mimalloc's is 1.250, tcmalloc's 1.231.

Reading: v2's table is arithmetic (16 B steps) to 512 B — the
hot range for real programs — and then steps that halve in
relative size per octave (32 B steps to 1 KB, 64 to 2 KB, …),
which is *denser* than the ~25 %-spaced geometric tables of
jemalloc/mimalloc/tcmalloc above 512 B. Only glibc beats v2 in
the mid range, because glibc's granularity is a flat 16 B at
every size; glibc pays for it in the small range instead (its
per-chunk header inflates 1–512 B requests to 1.176, the worst
of the five).

Two structural notes:

- **Span tail waste.** A 64 KiB span of class *c* wastes
  (65472 mod *c*) bytes of address space at its tail — worst for
  the largest small class (c = 32768: one block per span, ~50 %
  of the mapping is tail). Untouched tail pages are not resident,
  so this costs virtual space and page-table entries, not RSS;
  it is recorded, not fixed.
- **The large path (> 32 KiB)** is deliberately coarse (ADR
  0011/0019): page-rounded mappings with cached slack, governed
  by the per-thread cache budget (`FM_LCACHE_MAX_BYTES`, ADR
  0027). Its footprint is a retention question, already measured
  by the app matrix, not an internal-fragmentation one.

## Decision

**No class-table revision.** A revision could only chase
glibc's mid-range 1.003 by going to flat 16 B steps everywhere —
i.e. ~2048 classes to 32 KiB — which would multiply the
per-class state (central stacks, TLS lists, census surface) that
the current 128-class design keeps cache-resident, to recover at
most ~2 % on requests above 512 B while the allocators this
project benchmarks against sit at 7–9 %. The evidence says the
table is well-placed; the footprint battle is retained-free
memory (ADR 0020/0024) and live-set spread, and that is where
the rounds are spent. The comparison table above is the standing
evidence; `bench/usableprobe` re-measures it on demand.
