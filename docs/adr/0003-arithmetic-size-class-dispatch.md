# 0003 — Arithmetic size→class dispatch up to 512 B

*Status: Accepted. Provenance: Reconstructed (2026-11-12, from README
§2.2 and `src/fm_core.h`).*

## Context

A table lookup for size→class costs a load and a dependency on the
critical path. Most requests in real programs are small.

## Decision

Make the class ladder 16-byte granular up to 512 B so the mapping is a
pure shift, `class = (size + 15) >> 4`, with class numbering chosen so
no correction is needed (class 0 serves size 0; class c ≥ 1 serves
(16(c−1), 16c]). Above 512 B the ladder coarsens and a byte table
(`kClassBig`, values offset by 32) is used; that range is colder.

## Consequences

The small-size fast path needs no memory access for dispatch. Sizes
above 512 B pay one table load. This decision is also what the JIT
(0007) tries to beat by baking the answer into generated code.
