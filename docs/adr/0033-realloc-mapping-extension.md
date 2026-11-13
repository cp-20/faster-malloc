# 0033 — Realloc growth by mapping extension: built, measured, rejected

*Status: Rejected (2026-11-13). Contemporaneous. Round 6, item 4.
The prototype was implemented, strace-verified, and reverted
(commit af80cec) after the measurements below.*

## Background

ADR 0018 rejected mremap-based realloc growth three ways: the
design that moved the header was unsafe, and the safe version
measured 7.9× slower. The remnant the round-6 backlog kept is
narrower: when a large block is the only occupant of its mapping
and needs to grow, extend *the mapping* — never relocate
anything, never shrink anything.

The baseline cost of the status quo, on `bench/rgrow.c`
(40 KB → 32 MB by doubling, touching each new page): glibc
0.101 s, **v2 0.287 s**, mimalloc 0.411 s, jemalloc 3.362 s in
the pre-round session. v2's growth was alloc+copy every
doubling, and the large cache could not help: its bucket search
looks at the request's bucket and the next one up, so the block
freed by the previous doubling (2× the new request) is never
found for it.

## The prototype

In `fm_realloc`'s large, unaligned path, after the
fits-in-mapping check failed:

```c
mmap(map_end, extra, PROT_READ|PROT_WRITE,
     MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0)
```

Success at exactly `map_end` grows `sp->map_size` in place — no
copy, header unmoved. Any failure falls back to alloc+copy.
Correctness was never in question: the full battery passed with
it, and `MAP_FIXED_NOREPLACE` cannot clobber anything.

## Why it was rejected: the address is never free

The adoption rule for this item was "a clean win", and the
mechanism never gets to win. Strace counts of the extension
attempt on the final build:

| workload | attempts | successes |
|---|---|---|
| rgrow (200 rounds of doubling) | 1,600 | **0** |
| sqlite speedtest1 --size 30 | 355 | **0** |
| lua churn2 (peak delta) | — | no change (18,892 vs 18,936 KB) |

Every attempt failed with `EEXIST`. The reason is structural:
this allocator's heap is *densely packed by construction*. Small
spans are themselves 64 KiB mappings, large mappings are placed
by the kernel adjacently (top-down) against them and against
each other, and the large cache deliberately retains freed
mappings in the same neighbourhood. A growing block is, in
every real heap measured, never the topmost mapping — something
the allocator itself owns almost always sits at its end
address. The one heap shape where the extension would fire (a
lone growing buffer with free VA above it) did not occur in any
of the round's workloads.

glibc's rgrow advantage comes from a different mechanism —
mremap on chunks drawn from its own arena/mmap management,
where the growing chunk's surroundings are arranged by the same
allocator. Chasing that means revisiting ADR 0018's ground,
which this round does not do on the strength of one micro
benchmark: rgrow's absolute numbers (v2 ~0.20 s vs glibc ~0.04 s
on 2,200 growths of a synthetic doubler) matter to real programs
only through workloads like sqlite's, and sqlite's cell in the
app matrix is contention/noise-bound on this VM (ADR 0023's
finding stands).

## What survives

- The diagnosis of the large-cache bucket search (request bucket
  + 1 only) as the reason the growth trail never recycles: still
  true, still bounded by ADR 0019's cache budget, and the trail
  itself is quantified in ADR 0031 (~3.1 MB of lua's peak).
  Widening the realloc-path search is a smaller, separate idea —
  round-7 material, to be taken up only with a workload that
  shows it mattering.
- The strace methodology: counting mechanism attempts vs
  successes settled in one run what timing on this noisy VM
  could not (an apparent ~10% rgrow improvement between builds
  was unattributable once the success count proved to be zero).
