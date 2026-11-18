# 0076 — Dense span extents, locked batch-header pool, flush-paced regroup prompt

Accepted 2026-11-18.

## Evidence

`jq_large` (200k-record JSON, filter + `sort_by`) ran 1.48 s under v2 versus
1.14 s mimalloc and 1.24 s jemalloc (median of 7, interleaved, same host).
The heap is a bulk build of small objects followed by bulk free. Three
costs were found:

* every 64 KiB small span was its own `mmap`, so the build phase paid one
  VMA plus base-page faults per span;
* the fragmentation regroup prompt (ADR 0072) re-walked the whole retained
  pool every 128 flushes of *any* class during bulk free;
* the global batch-header freelist was a lock-free Treiber stack whose nodes
  are recycled, i.e. ABA-prone. On main, `micro threads 8 64 5000000`
  hangs or segfaults in 9 of 40 runs; after this change 0 of 70.

## Decision

1. **Dense extents.** Full-size spans of classes ≤ 4 KiB are carved from a
   2 MiB-aligned extent under the central lock. Each span still covers its
   own range and is released on its own. After four warm-up extents, an
   extent consumed within 100 ms gets `MADV_HUGEPAGE` for the next one, so
   sparse or start-up heaps keep demand-paged base pages. `fm_trim_spans`
   unmaps the unused rest of the current extent.
2. **Locked batch pool.** `fm_batch_global_head` is protected by
   `fm_central_lock` (already fork-aware). A released header goes back to
   the thread-local pool first when that pool is empty, so the common
   publish/pop pair takes no lock. Thread exit returns the local pool to
   the global list.
3. **Paced prompt.** The early regroup prompt runs only for the class being
   flushed, and only every `prompt_mask + 1` flushes. `fm_trim_class` sets
   the mask from the free supply it just rebuilt: the stride doubles from
   128 flushes until it exceeds half the flushes that supply represents.
   Timed release and explicit trim keep their cadence. A time-based
   throttle was tried and rejected because it regressed the locality probe.

## Result (median of 7, interleaved; with ADR 0075)

| workload | v2 before | v2 after | mimalloc | jemalloc |
|---|---|---|---|---|
| jq_large | 1.480 s / 252 MB | **1.111 s** / 259 MB | 1.137 s / 272 MB | 1.243 s / 277 MB |
| python_data | 0.399 s / 150 MB | 0.382 s / 143 MB | 0.367 s / 146 MB | 0.395 s / 159 MB |

Micro (ns/op, min of 5–11): batch1024 134 → 79, rand 90 → 76, churn 5.8 → 5.5,
large 13.7 → 12.8, realloc 3.50 → 3.35. The individual contribution of each
of the three mechanisms was not measured separately.
