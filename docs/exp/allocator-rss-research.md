# Allocator RSS mechanisms — survey for faster-malloc

Round 13 made v2 first on time for most apps but not on peak RSS.  This
note records how the established allocators keep RSS down, and which
mechanisms are worth porting to v2.  It is a working note for the RSS
campaign, not an ADR.

## What the others do

### glibc malloc

* Small allocations come from the main heap grown with `brk`; it is
  contiguous, so a short process has one mapping and only touched pages
  are resident.
* Large allocations are `mmap`ed and `munmap`ed immediately (the dynamic
  mmap threshold adapts upward only when freed mmaps are seen).
* Free chunks stay in bins/tcache; `malloc_trim` walks and
  `MADV_DONTNEED`s free pages, but the default does not call it often.
* Per-thread arenas (up to 8 × cores) reduce contention; each arena can
  hold a lot of free memory, which is why glibc is not always the RSS
  winner on threaded workloads.

Relevant to v2: the low RSS of glibc on short processes is mostly **one
contiguous heap + immediate munmap of large blocks**, not a clever cache.

### jemalloc

* Arenas, size classes, per-thread tcaches, and extents.
* Dirty pages are decayed: `dirty_decay_ms` / `muzzy_decay_ms` release
  pages after a time window, optionally by a background thread.
* `madvise(MADV_FREE)` (muzzy) keeps the virtual mapping but lets the OS
  reclaim the physical page; `MADV_DONTNEED` (retained) drops it
  immediately.
* `opt.retain` controls whether unused extents are kept; `arena.<i>.purge`
  forces return.

Relevant to v2: **time-based decay with a background/idle cadence** is the
missing piece.  v2's census runs on flush events and `malloc_trim`; a
process that frees a lot and then computes (sort merge, compiler pause)
keeps the pages.

### mimalloc

* Segments (4 MiB / 32 MiB) of pages of one size class; per-thread heaps.
* Pages are "abandoned" to the freeing thread, so cross-thread frees keep
  locality; empty pages are returned to the segment.
* `mi_option_purge_delay` / `mi_option_purge_decommits` control delayed
  purging; `MADV_FREE` is used where available.
* Segment retirement returns whole segments; small objects are packed
  into pages, so the metadata overhead is low.

Relevant to v2: **page-granular state and delayed purge** are already
close (ADR 0030); the missing part is a **decay timer** and a cheap way
to purge a page range without walking every span.

### tcmalloc

* Size classes, per-thread caches, central page heap; spans of pages.
* `ReleaseFreeMemory` / `ReleaseToOSIntervalMs` background release.
* Huge-page aware allocation (`MADV_HUGEPAGE`) and `madvise` release.

Relevant to v2: tcmalloc's `ReleaseToOSIntervalMs` is the same idea:
**periodic release with a tunable interval**, not a per-free decision.

## What already exists in v2

* `FM_RELEASE_*` automatic census + `MADV_DONTNEED` page purge (ADR 0020,
  0030, 0049, 0071).
* `FM_LCACHE_COLD_PURGE`: drops payload pages of cold cached large blocks
  on free (ADR 0058 measured it as a guard regression when applied to
  every admission; the round-13 test applies it only to cold buckets and
  measured **sort 45.7 -> 34.0 MiB with time neutral**).
* `FM_LOCALITY_SORT`, `FM_LAZY_BUMP`, profiles.

## Mechanisms worth trying next

1. **Decay timer / idle purge (highest value).**  After N ms with no
   allocation activity, purge cold large-cache payloads and fully-free
   span pages.  jemalloc's `dirty_decay_ms` and tcmalloc's
   `ReleaseToOSIntervalMs` are the reference.  v2 already has an opt-in
   idle thread (ADR 0032/0036); make a cheaper per-thread check on the
   slow path, or arm the idle thread only after a large free burst.
2. **MADV_FREE for muzzy pages.**  Keep the mapping and drop physical
   pages under pressure; cheaper than unmapping and re-mapping.  Test on
   sort/xz (large temporary buffers) and Python (arena churn).
3. **Page-run purge for small spans.**  Instead of a full census walk,
   track per-span free page masks (ADR 0030 already has `purged_mask`)
   and purge a page as soon as its last block is freed, bounded by a
   per-span threshold.
4. **Startup mapping diet.**  glibc's advantage on git/sqlite is one
   heap.  v2 maps a 2 MiB dense extent, a 4 MiB slab, and 16 KiB first
   spans per class.  A short-lived process should use a small bootstrap
   area and only open the dense extent/slab after a second request.  The
   first attempt (4 KiB first spans) increased RSS because it created
   more spans; the lesson is to **avoid spans, not shrink them**.
5. **Return whole spans on thread exit / process idle.**  The existing
   `fm_thread_exit_flush` unmaps only the thread's own residue; central
   and other threads' spans remain.  A trim-on-idle cadence can close
   the Python/Node gap.
6. **Avoid retaining large realloc headroom at the peak.**  ADR 0080's
   reserve is VA-only until used, but a grown buffer carries it into the
   cache.  Cap the reserve for buffers that are freed without reuse.
7. **Match glibc's immediate munmap for one-shot large blocks.**  The
   hot-window already does this for teardown buckets; make the window
   shorter for >8 MiB blocks and longer for 32-128 KiB reuse.

## Source pointers

* jemalloc tuning: <https://jemalloc.net/jemalloc.3.html> (`dirty_decay_ms`,
  `muzzy_decay_ms`, `retain`, `background_thread`).
* mimalloc options: <https://microsoft.github.io/mimalloc/group__options.html>
  (`purge_delay`, `purge_decommits`, `eager_commit`).
* tcmalloc release: `MallocExtension::ReleaseFreeMemory`,
  `ReleaseToOSIntervalMs`.
* glibc malloc: `malloc/malloc.c` (`M_TRIM_THRESHOLD`,
  `M_MMAP_THRESHOLD`, `systrim`, `heap_trim`).
