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
| [0010](0010-deferred-memory-return-and-shaping.md) | Deferred: memory return to the OS and free-list shaping | Deferred |
| [0011](0011-large-block-cache.md) | Cache recently freed large blocks instead of munmap/mmap per operation | Accepted |
| [0012](0012-calloc-skip-memset-on-fresh-pages.md) | calloc: skip the memset for memory known to be zero | Deferred after measurement |
| [0013](0013-server-macro-variance-investigation.md) | Server macro: apparent v2 loss to v1 investigated | Closed — measurement noise, no change |
