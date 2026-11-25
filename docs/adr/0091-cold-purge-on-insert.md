# 0091 — Purge a cold cached block on insert

Proposed 2026-11-25.

## Context

The selective large-block cache (ADR 0087) keeps at most one block per
bucket that has never served a hit (`FM_LCACHE_BUCKET_COLD`, 256 KiB).
That block is a guess: the bucket never hit, and Round 32 measured sqlite
paying 1.7 MiB for it (`FM_LCACHE_BUCKET_COLD=0` took sqlite
11.1 -> 9.4 MiB at +15% time, so dropping the block entirely is too
expensive).

The block's only value is that a later reuse skips the `mmap`.  Its pages
have no value until that reuse happens, and a cold bucket's reuse is by
definition rare.

## Decision

Keep the block (and its mapping) but return its payload pages at insert
time with `MADV_DONTNEED`.  Page 0 holds the cache header and stays
resident.  A later reuse faults the pages back -- exactly the cost the
`mmap` path would have paid, minus the syscall.

```c
if (!warm) {
    uintptr_t pstart = (uintptr_t)sp + FM_PAGE_SIZE;
    uintptr_t pend = (sp->map_base + sp->map_size) & ~(FM_PAGE_SIZE - 1);
    if (pend > pstart)
        (void)madvise((void *)pstart, pend - pstart, MADV_DONTNEED);
}
```

The change is confined to the large-free slow path: no allocation, free,
refill or lookup path changes.

## Evidence

11-rep exp-bench (small fixtures, few cold blocks): micro **1.0003 /
worst 1.0004**, real paired 1.0021, RSS flat -- i.e. the change costs
nothing where there is nothing to purge.

Local fixtures where cold blocks matter: gcc `-O1 -c sqlite3.c`
370.1 -> 350.5 MB, lua 16.2-16.6 -> 14.7-15.0, sqlite speedtest1
11.5-11.7 -> 11.07-11.14, git and node unchanged.

A paired 11-rep run of the exact legacy gcc workload (interleaved, medians):
RSS 370,212 -> 351,680 KiB (**-18.1 MiB**) at a paired time ratio of
**1.0123** (+1.2%).  gcc's legacy cell should therefore land near
340 MiB against glibc's 342.3, i.e. below glibc for about 1% of time.

`test_basic` (both libraries), `stress`, `rtest`, `valgrind`,
`mtrepeat` and `abicheck` pass.

## Risks

* A cold block that *is* reused pays page faults instead of a cached
  page; the exp-bench time cells are the guard (they are neutral).
* One `madvise` per cold free is added to the large-free path.
* The pages are zero-filled after the purge, so a reuse must not assume
  the old contents; `fm_large_alloc_search` already reports such blocks
  as not zeroed and calloc memsets them.
