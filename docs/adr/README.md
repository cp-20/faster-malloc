# Architecture Decision Records

One coherent record of the design and experiment decisions behind
faster-malloc: the original design history (reconstructed from the README
and code) and the new experiments from 2026-11-12 onwards (recorded as
they happened).

**Provenance:** entries marked *Reconstructed* were written after the
fact, on 2026-11-12, from the README, the design notes in the code, and
the recorded measurements. They describe the rationale as recorded in
those sources; they are not contemporaneous notes. Entries without that
mark were recorded when the work was done.

| # | Title | Status |
|---|---|---|
| [0001](0001-v1-baseline-allocator.md) | v1 as a conventional baseline allocator | Superseded by 0002–0006 |
| [0002](0002-spans-no-per-block-headers.md) | Spans with header-only metadata; no per-block headers | Accepted |
| [0003](0003-arithmetic-size-class-dispatch.md) | Arithmetic size→class dispatch up to 512 B | Accepted |
| [0004](0004-parallel-tls-arrays-frameless-fast-path.md) | Parallel TLS arrays and frameless hot paths | Accepted |
| [0005](0005-lockfree-central-list-of-batches.md) | Lock-free central list of whole batches | Accepted |
| [0006](0006-local-cache-limit.md) | Per-thread local cache limit of 256 blocks, size-bounded | Accepted |
| [0007](0007-jit-value-specialisation.md) | JIT value specialisation via patched assembler template | Accepted (experimental) |
| [0008](0008-jit-concentration-heuristic.md) | JIT concentration heuristic: specialise only if hottest class ≥ 50 % | Accepted |
| [0009](0009-measurement-methodology.md) | Measurement methodology: instruction counts first, wall clock on real programs | Accepted |
| [0010](0010-deferred-memory-return-and-shaping.md) | Deferred: memory return to the OS and free-list shaping | Partially resolved by 0014/0015 |
| [0011](0011-large-block-cache.md) | Cache recently freed large blocks instead of munmap/mmap per operation | Accepted |
| [0012](0012-calloc-skip-memset-on-fresh-pages.md) | calloc: skip the memset for memory known to be zero | Deferred after measurement |
| [0013](0013-server-macro-variance-investigation.md) | Server macro: apparent v2 loss to v1 investigated | Closed — measurement noise, no change |
| [0014](0014-free-list-shaping.md) | Free-list shaping: span-grouped recycling into a per-thread stash | Rejected after measurement |
| [0015](0015-malloc-trim-span-census.md) | malloc_trim that actually returns spans: the central-list census | Accepted |
| [0016](0016-entries-callable-before-constructor.md) | Entry points must be callable before the library constructor runs (Node.js/JIT startup crash) | Accepted |
| [0017](0017-thread-exit-drain.md) | Drain per-thread caches when a thread exits | Accepted |
| [0018](0018-mremap-realloc-growth-rejected.md) | Realloc growth via mremap: rejected (the cache already does this job) | Rejected after measurement |
| [0019](0019-large-cache-budget.md) | Large-block cache budget: 4 GiB → 64 MiB | Accepted |
| [0020](0020-automatic-span-release.md) | Automatic small-span return: the census on a cadence, with dead-span ageing | Accepted (instant-release form rejected inside) |
| [0021](0021-refill-side-regrouping-rejected.md) | Refill-side regrouping: measured, and rejected like its flush-side parent | Rejected after measurement |
| [0022](0022-jit-jump-table-rejected.md) | JIT jump-table dispatch: implemented, measured, rejected | Rejected after measurement |
| [0023](0023-sqlite-node-gap-investigation.md) | The sqlite and node "gaps": profiled; no allocator defect found | Closed — investigation record |
| [0024](0024-occupancy-aware-placement.md) | Occupancy-aware placement: the census rebuild regroups survivors by span | Accepted |
| [0025](0025-cross-scenario-ring-race.md) | bench/micro `cross` measured its own race; the ring is now bounded | Accepted (harness fix) |
| [0026](0026-release-constant-sweep.md) | Release-constant sweep: the defaults sit on a flat optimum | Accepted |
| [0027](0027-runtime-configuration-profiles.md) | Runtime configuration: FM_PROFILE and per-knob FM_* overrides | Accepted |
| [0028](0028-size-class-audit.md) | Size-class audit: the table is already well-placed; waste lives elsewhere | Accepted (no change) |
| [0029](0029-idle-process-release.md) | Idle-process release: design pass, implementation deferred | Deferred |
| [0030](0030-page-granular-purge.md) | Page-granular purge in partially-free spans | Accepted |
| [0031](0031-lua-peak-decomposition.md) | Lua peak decomposition | Accepted (measurement) |
| [0032](0032-zero-trickle-and-idle-release.md) | Zero-trickle retention and idle-release prototype | Accepted, opt-in; hardened by 0036 |
| [0033](0033-realloc-mapping-extension.md) | Realloc mapping extension | Rejected |
| [0034](0034-realloc-cache-search.md) | Realloc large-cache bucket search | Opt-in prototype; default promotion rejected |
| [0035](0035-purge-contention-ownership.md) | Purge contention: census and batch ownership | Accepted correctness fix; performance provisional |
| [0036](0036-idle-timer-fork-signals.md) | Idle timer fork and signal ownership | Accepted hardening; default-off |
