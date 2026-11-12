# 0001 — v1 as a conventional baseline allocator

*Status: Superseded by 0002–0006 (v2). Provenance: Reconstructed
(2026-11-12, from README §5–§7 and `src/alloc_v1.c`).*

## Context

The project set out to test ideas from the allocator/JIT literature
(tcmalloc-style caches, mimalloc-style page locality, value
specialisation) against conventional designs. A straightforward
segregated-free-list allocator was needed as the in-house baseline so
that later optimisations could be measured against something with the
same build and harness.

## Decision

Implement v1 (`src/alloc_v1.c`, `libfastmalloc.so`) as a conventional
segregated size-class allocator with per-thread caches, and keep it in
the tree permanently as the baseline every later allocator is compared
against.

## Consequences

v1 already beats glibc on most micro workloads (e.g. 21.98M vs 42.75M
instructions on churn 64 B), which made it a meaningful floor: v2's wins
are measured against v1, not only against glibc. It remains in every
benchmark table.
