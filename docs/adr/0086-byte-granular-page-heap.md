# 0086 — Byte-granular page heap for the 257-2032 B band

Proposed 2026-11-25.

## Context

Every RSS cell outside jq is above glibc, and the gap is not live bytes:
a `livepeak` interposer measured 11.81 MB of peak live malloc bytes for
v2 against glibc's 11.65 MB on the legacy git fixture, while peak RSS was
23.9 against 19.6 MB.  The difference is page fragmentation between size
classes: a class that keeps a handful of blocks resident pins whole 4 KiB
pages that no other class can use, and no free-page knapsack recovers
them (page purge is a no-op on git/lua/sqlite, and fully-free spans are
released already).

A trace-driven simulation put the ceiling of perfect byte-granular
packing at 9.04 MB (lua) and 12.04 MB (git) against glibc heaps of 9.18
and 11.1 MB: the best possible shared-page design is glibc-level RSS.

## Decision

For requests in `[257, 2032]` bytes, v2 takes blocks from 4 KiB pages
carved into variable-length chunks instead of class spans:

* 16-byte chunk header (size in bytes with a free bit) and 16-byte-aligned
  payloads, so `malloc` still returns 16-byte-aligned pointers;
* size-agnostic free buckets (16-byte steps) with a non-empty bitmap and
  doubly-linked lists, forward coalescing inside a page;
* a per-thread 16-entry cache in front of the global heap, which is
  protected by one mutex;
* pages come from a lazily reserved 1 GiB `PROT_NONE` arena, committed
  4 KiB at a time with `mprotect`;
* sizes below 257 B stay on v2's bins (lua's small-object churn is faster
  there) and sizes above 2032 B stay on the span path.

The whole block is behind `FM_PAGEHEAP` (enabled on the experiment
branch).  `fm_free`, `fm_realloc` and `malloc_usable_size` route by
address range first, so both allocators coexist.

## Evidence (local fixtures, back-to-back)

| case | v2 | page heap | glibc |
|---|---:|---:|---:|
| git rev-list | 23.9 MB / 0.04 s | **21.1 MB / 0.04 s** | 19.7 MB / 0.05 s |
| sqlite speedtest1 | 11.6 MB / 2.86 s | **10.9 MB / 2.84 s** | 8.3 MB / 2.6 s |
| lua churn | 16.3 MB / 0.33 s | 16.3 MB / 0.33 s | 11.4 MB / 0.42 s |

`test_basic`, `bench/stress`, `rtest` (8 threads, 512 B -> 512 MiB with
pattern checks), `valgrind` stress and `mtrepeat` pass.

## Risks

* The heap has one global mutex; multi-threaded churn in this band
  serialises.  The band is 257-2032 B, so hot small-object traffic stays
  on the lock-free bins.
* Per-thread caches hold up to 16 chunks per size; they are drained at
  thread exit.
* A request just above the band (2033 B) still pays span fragmentation,
  and the measured lua trace shows the band split is what keeps lua
  neutral.
