# 0077 — Per-thread large slab, cached-mapping splitting, and dense first spans

Accepted 2026-11-18.

## Evidence

After ADRs 0075 and 0076, `python_data` (`py_pipeline.py`) still trailed
`mimalloc` and `tcmalloc` by ~12–15 ms on wall clock despite executing fewer
user-space instructions (`Valgrind Callgrind` on a 1/10th workload: v2
382.4M Ir vs glibc 385.5M Ir and mimalloc 387.0M Ir). Profiling with
`strace -c -f`, `/usr/bin/time -v`, and per-phase `CLOCK_PROCESS_CPUTIME_ID` +
`getrusage` isolated the gap entirely to kernel VM and page-fault churn:

* **515 `munmap`, 408 `mmap`, and 24 `madvise` syscalls** in v2 versus
  108 `munmap`, 176 `mmap`, and 6 `madvise` in mimalloc (where CPython's own
  1 MiB pymalloc arenas and `ld.so` account for ~108 `munmap` and ~170 `mmap`).
* **315 `mmap`/`munmap` syscalls** came from `FM_SMALL_FIRST_SPAN` calling
  3-syscall `fm_map_aligned` for the first 16 KiB span of 105 startup classes,
  and another **102 syscalls** came from >4 KiB classes (`group`'s 29 spans of
  8 KiB blocks) bypassing `fm_map_dense_span`.
* **64 `mmap` syscalls** came from `bytearray` allocations of 128–176 KiB
  missing the empty bucket-17 cache while **6.8 MiB of freed mappings** from
  `recs`, `json.dumps`, and `json.loads` sat idle in larger cache buckets.
* **`lcache_cold_purge = 1`** (`MADV_DONTNEED` on the first free in each cold
  bucket) zapped 6.8 MiB of resident growth rungs right before `dumps`/`loads`
  reused them (+1,469 minor faults) and zapped 18 MiB of hugepages at process
  teardown right before `exit_group`.
* **5 automatic censuses** during teardown mapped, faulted, and unmapped
  45 KiB census tables (+50 minor faults).

## Decision

1. **Per-thread 2 MiB base-page slab (`fm_map_large_slab`).** Classes with
   `bs > 4 KiB` and plain large allocations up to 512 KiB carve 64 KiB-aligned
   ranges from a thread-local 2 MiB anonymous mapping without per-allocation
   `mmap`/trim syscalls and without `MADV_HUGEPAGE`. (`MADV_HUGEPAGE` on
   multi-span large extents was tested and rejected: sparse-touch workloads
   such as `micro pareto` faulted full 2 MiB folios — 166 MB → 466 MB RSS —
   and paid `split_folio` costs on overflow `munmap`, 3.5 µs → 8.6 µs/op.)
   `fm_large_cache_drain` unmaps the unused slab tail on `malloc_trim` and
   thread exit.
2. **Dense VA slots for first spans.** Under `FM_SMALL_FIRST_SPAN`, classes
   `bs ≤ 4 KiB` still cap `sp->nblocks` on their first span to 16 KiB worth of
   blocks (preserving startup RSS) while carving the 64 KiB VA slot from
   `fm_map_dense_span()` instead of 3-syscall `fm_map_aligned`.
3. **Bounded large-cache splitting and warm retention.** On a miss in
   `fm_large_alloc_map` for requests ≤ 2 MiB (when wide search is off), split
   cached mappings up to 4 MiB at a 64 KiB boundary, returning the resident
   prefix and re-bucketing the remainder. Default `lcache_cold_purge` is 0 in
   `balanced` (`footprint` retains 1).
4. **Reusable census table.** `fm_census_init` retains its `mmap`ped hash table
   under `fm_census_lock` across automatic censuses and unmaps it on explicit
   `malloc_trim`.

On `python_data`, syscalls drop from 408 → 196 `mmap`, 515 → 223 `munmap`, and
24 → 13 `madvise` (total `strace -c` syscall time 45.0 ms → 19.4 ms, below
mimalloc's 21.1 ms), and minor faults fall from 35,549 → 34,080 (mimalloc
35,635).

## Result (`ab.py`, median of 7 interleaved runs)

| Workload | v2 before (`0622d29`) | v2 after | mimalloc | jemalloc | tcmalloc | glibc |
|---|---|---|---|---|---|---|
| python_data | 0.413 s / 143 MB | **0.400 s** / 145 MB | 0.435 s / 146 MB | 0.424 s / 159 MB | 0.441 s / 153 MB | 0.410 s / 141 MB |
| jq_large | 1.318 s / 259 MB | **1.199 s** / 263 MB | 1.318 s / 272 MB | 1.415 s / 277 MB | 1.388 s / 274 MB | 1.583 s / 264 MB |
| sort | 0.142 s / 30 MB | **0.139 s** / 30 MB | 0.151 s / 30 MB | 0.162 s / 32 MB | 0.154 s / 36 MB | 0.147 s / 29 MB |
| xz | 8.115 s / 93 MB | 8.641 s / 93 MB | 8.498 s / 107 MB | 8.602 s / 96 MB | 8.729 s / 112 MB | 8.496 s / 92 MB |
| git | 0.017 s / 8 MB | **0.016 s** / 8 MB | 0.016 s / 8 MB | 0.019 s / 10 MB | 0.024 s / 14 MB | 0.015 s / 7 MB |

Micro (`bench/micro`, min of 5, pinned): `pareto` 7123 → 5929 ns/op (RSS
167.0 → 166.9 MB), `batch1024` 174 → 151 ns/op, `rand` 1335 → 386 ns/op,
`cross` 48.0 → 39.0 ns/op; `churn`, `large`, `realloc`, `calloc`, `touch`, and
`threads` at parity (Callgrind `Ir/op` within 0.02 Ir/op across small-object
modes). All correctness, thread-exit, lifecycle, JIT, and `mtrepeat.sh` gates
pass on both `libfastmalloc2.so` and `libjitmalloc.so`.
