# 0018 — Realloc growth via mremap: rejected (the cache already does this job)

*Status: Rejected (2026-11-12). Recorded contemporaneously.*

## Context

Large-block `realloc` growth was allocate-copy-free. A new probe,
`bench/rgrow` (malloc 40 KiB, realloc doubling to 32 MiB, one byte
touched per page, 200 rounds), showed the cost plainly:

| rgrow | time |
|---|---:|
| glibc | 0.036 s |
| v2 (allocate-copy-free) | 0.294 s |

glibc is fast here because it grows mmap'd chunks with `mremap`.
Three variants of the same idea were tried for v2.

## Variant A — fits-check (accepted, kept)

If the existing mapping already has room — common after a
large-cache reuse hands out an oversized mapping — return the
pointer unchanged, measuring capacity from the user pointer to the
end of the mapping. This is not really mremap, just not copying
when no copy is needed; it also covers every shrink. Pointer
stability is asserted by a regression test in `test_basic`
(200 KiB cached → 128 KiB alloc → realloc to 150 KiB must not
move). `fm_map_large` now records the page-aligned mapping length
so capacity accounting is exact.

## Variant B — mremap without MREMAP_MAYMOVE (neutral, dropped)

Extending the VMA in place almost never fires in a live heap: the
address range after a fresh large mapping is usually occupied, if
only by this allocator's own cached blocks. rgrow 0.289 s vs
0.294 s, Lua churn 0.608 s vs 0.602 s — noise both ways. Dropped
with the rest of the mremap direction to keep the code simple.

## Variant C — mremap with MREMAP_MAYMOVE (unsafe as sketched, slow as fixed)

The obvious sketch — `mremap(..., MREMAP_MAYMOVE)`, then rebuild
the header at `new_p & SPAN_MASK` — is **wrong**, and this is the
important catch of the experiment: after a move, the masked span
base can precede the new mapping. Forced-move counterexample
measured during the experiment: `new_raw = 0x...c35000`,
`new_p = 0x...c35040`, masked header `0x...c30000` — 20 KiB
*before* the mapping, in address space that may be unmapped or
belong to another live block. (The user offset modulo the span
size stays in [64, 64 KiB), which proves the header would precede
the user data; it does not prove the header address is inside the
moved mapping, because the mapping itself is not span-aligned.)

The safe version — reserve `want + SPAN_SIZE`, `MREMAP_FIXED` the
block into a span-aligned destination inside the reservation, trim
the reservation's head/tail, rebuild the header from a stack copy —
passed the whole correctness battery plus a dedicated relocation
probe (all 16 page phases, live neighbour block, repeated
move/shrink/grow). It was still rejected on performance:

| workload | before | MAYMOVE/FIXED |
|---|---:|---:|
| rgrow | 0.307 s | **2.416 s (7.9× worse)** |
| Lua churn | 0.644 s | 0.602 s (6.5% better) |
| rgrow minor faults | ~10,300 | **1,022,489** |

Why: in the allocate-copy path, each freed rung of the growth
ladder lands in the large-block cache, and the next round's ladder
reuses those mappings *with their pages already resident* — the
cache is playing exactly the role glibc's retained heap top plays
for the same benchmark (glibc: ~10,307 faults, like our before).
A moved block bypasses that cycle: it is either cache-ineligible
on free (header no longer at `sp + HEADER_SIZE`) or leaves no
intermediate mappings behind at all, so every round re-faults
every page it grows into. The 6.5% Lua gain (single ever-growing
string — the one pattern that truly matches mremap) does not pay
for a 7.9× loss on ladder-style growth, which is what growable
buffers in most runtimes actually do.

## Decision

No mremap in `realloc`. Growth is: fits-check (A), else
allocate-copy-free through the large cache. If a future workload
profile shows single-block growth dominating *and* ladder reuse
absent, variant C is the known-safe way to revisit this — with its
fault economics measured first, per ADR 0012's lesson.
