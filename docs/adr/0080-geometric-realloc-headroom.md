# 0080 — Reserve headroom for geometric large-realloc growth

Accepted 2026-11-24.

## Context

ADR 0075 added a reserve to `fm_large_alloc_map()` for a buffer that grows
in *sub-doubling* steps (`size - old < old`).  Parsers and builders usually
grow geometrically instead (1.5x or 2x), so `size - old >= old` held and
every step copied the payload into a fresh mapping.  jq's arrays and the
sort/xz buffer growth paths paid a copy plus fresh page faults at each
step.

## Decision

Any large growth now reserves one extra request of headroom, bounded at
8 MiB, in both the cached-carve path and the fresh-mapping path:

```c
if (growing_from) {
    size_t extra = size < (8 MiB) ? size : (8 MiB);
    capacity = size + extra;
}
```

`sp->user_size` is still the actual request, so `malloc_usable_size()` and
the realloc in-place check stay honest.  If the larger mapping fails, the
existing exact-request retry is used.  mremap is still not used
(ADR 0018/0033 remain rejected).

## Evidence

`exp/realloc-growth`, 7 repetitions, CI runner:

| case | before | after | ratio |
|---|---:|---:|---:|
| `sort_p4` median | 0.175 s | 0.161 s | 0.920 |
| `sort_p4` min | 0.173 s | 0.158 s | 0.913 |
| `xz_mt2` median | 1.997 s | 1.912 s | 0.957 |
| `jq_large` median | 0.913 s | 0.919 s | 1.007 |
| real-app geomean | — | — | **0.9831** |
| micro geomean (Ir) | — | — | 1.0000 |

In the combined branch (with ADRs 0079, 0081, 0082) the same suite at 11
repetitions measured `sort_p4` 0.138 -> 0.126 s (paired 0.905) and a
real-app paired geomean of 0.982.

The 7% higher `sort` peak RSS (42 -> 45 MiB) is the one visible cost: the
reserved pages are VA-only until grown into, but the mapping and its
`map_size` accounting are larger.

## Risks

* A grown buffer carries its reserve into the large-block cache, so the
  64 MiB budget holds fewer entries.  ADR 0081 compensates for warm
  buckets.
* Very large geometric buffers reserve up to 8 MiB of extra VA; a
  RLIMIT_AS-constrained process relies on the exact-size retry.
* Requests just below the 512 KiB slab threshold can now exceed it after
  doubling and fall back to a plain mapping.
