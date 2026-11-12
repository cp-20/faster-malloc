# 0005 — Lock-free central list of whole batches

*Status: Accepted. Provenance: Reconstructed (2026-11-12, from README
§2.5 and `src/fm_core.h`).*

## Context

Central-list traffic must be amortised: a lock or CAS per refill is
fine, one per free is not. Walking a free list to find its tail touches
every block, and free-list blocks are scattered across the heap, so each
step is a likely cache miss.

## Decision

The per-class central list is a Treiber stack of *batches* — whole free
lists, not fragments. A thread's local list keeps head, tail and count;
publishing is three stores plus one CAS, and a refill takes a whole
batch with one CAS. Batch headers come from a per-thread pool carved
128 per mmap'd page (so the pool itself does not recurse into malloc).
No lock is ever taken on the allocation path.

## Consequences

Both refill and flush are O(1). Cross-thread frees are handled by the
freeing thread adopting the block into its own list and publishing
batches, which is why v2 leads the cross-thread-free measurements
(README §5.4). Any publication-order bug here corrupts the heap, so the
batch invariants are exercised by `bench/check.c` and `bench/stress.c`.
