# 0033 — Realloc growth by mapping extension (no copy, no move)

*Status: Accepted (2026-11-13). Contemporaneous. Round 6, item 4.*

## Background

ADR 0018 rejected mremap-based realloc growth three ways: the
design that moved the header was unsafe, and the safe version
measured 7.9× slower. The remnant the round-6 backlog kept is
narrower: when a large block is the only occupant of its mapping
and needs to grow, extend *the mapping* — never relocate
anything, never shrink anything.

The baseline cost of the status quo, on `bench/rgrow.c`
(40 KB → 32 MB by doubling, touching each new page; quiet-box
session, 60 rounds): glibc 0.101 s, **v2 0.287 s**, mimalloc
0.411 s, jemalloc 3.362 s. v2's growth was alloc+copy every
doubling, and the large cache could not help: its bucket search
looks at the request's bucket and the next one up, so the block
freed by the previous doubling (2× the new request) is never
found for it.

## Decision

In `fm_realloc`'s large, unaligned path, after the existing
fits-in-mapping check fails, try:

```c
mmap(map_end, extra, PROT_READ|PROT_WRITE,
     MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0)
```

- On success at exactly `map_end`, the mapping is longer:
  `sp->map_size` grows, `user_size` updates, the same pointer is
  returned. No bytes copied, header unmoved, nothing else in the
  allocator changes — the block later frees, caches, and unmaps
  by its recorded `map_size` as usual.
- On any failure (something already lives at the end address),
  fall back to the pre-existing alloc+copy path unchanged.
  `MAP_FIXED_NOREPLACE` guarantees the probe can never clobber
  an existing mapping.

Scope limits kept deliberately: unaligned large blocks only
(aligned allocations have a different header geometry), growth
only, extension sized exactly to the page-rounded need (the
caller that keeps doubling pays one syscall per doubling and no
copy; VA headroom games are not this item).

## Expected effects beyond rgrow

ADR 0031 found ~3.1 MB of lua's peak was the large cache holding
realloc-doubling's superseded blocks. A block that extends in
place leaves no superseded block: the trail component of
grow-by-realloc workloads (sqlite's page cache growth is the
same shape) should shrink wherever the adjacent VA is free,
which for a mostly-private heap is the common case.

## Measurements

Recorded in the round-6 log with the full battery: rgrow before/
after on a quiet box, the realloc rows of the micro suite, and
sqlite + lua cells from the app matrix. (Battery: test_basic
incl. profiles, stress ×3, check, mt both modes, earlyinit,
tlsexit — green; FM_DEBUG build clean. The extension path is
exercised by test_basic's large-realloc cases and by rgrow
itself, whose checksum-equivalent output is unchanged.)
