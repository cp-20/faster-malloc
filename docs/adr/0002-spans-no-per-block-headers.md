# 0002 — Spans with header-only metadata; no per-block headers

*Status: Accepted. Provenance: Reconstructed (2026-11-12, from README
§2.1 and `src/fm_core.h`).*

## Context

Design conclusions from the literature review (README §1): the free list
must not live in front of the user's data (that wastes memory or risks
corrupting user bytes), and `free()` must find the size class without a
dependent load before it can reach the block.

## Decision

Allocate small objects from 64 KiB spans. A span is 64 KiB-aligned,
serves exactly one size class, and carries a single 64-byte header
(class, magic, block count, mapping info). There is no per-block
metadata at all. `free()` recovers the class with one AND (span base)
and one load (header), and large allocations (> 32 KiB) are individual
mmaps identified by a magic class value in the aligned header page.

## Consequences

The 64-byte header is amortised over the whole span; user pointers are
16-byte aligned with nothing stored in front of them. The cost is
internal fragmentation per span and the fact that spans are never
returned to the OS (see 0010).
