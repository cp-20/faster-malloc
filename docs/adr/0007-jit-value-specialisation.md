# 0007 — JIT value specialisation via a patched assembler template

*Status: Accepted (experimental). Provenance: Reconstructed
(2026-11-12, from README §3.1–§3.2 and `src/fm_jit.c`,
`src/jit_stubs.S`).*

## Context

Real programs concentrate nearly all allocations on a handful of exact
sizes. A static allocator commits to one size→class mapping for every
program; a JIT can profile the running program and generate a dispatch
specialised to its hot classes, with the TLS displacement baked in as
an immediate.

## Decision

The JIT build (`libjitmalloc.so`) profiles request sizes (sampled
1-in-64), then generates a specialised dispatch from an
**assembler-written template** (`src/jit_stubs.S`) by patching only
immediates: hot-class bounds, TLS displacements, the generic-fallback
address, and unused-slot branches. The generated code is installed
behind an atomically updated pointer. The template is copied, never
executed in place, and every branch inside it is relative so it stays
valid when relocated; the C side verifies the opcode bytes at every
patch point before installing.

Hand-assembling x86-64 in C was tried first and abandoned: a single
wrong ModRM byte produced code that faulted only on some inputs and
looked like random heap corruption (recorded in README §3.2).

## Consequences

The JIT wins 1–9 % of instructions on monomorphic workloads and is
never significantly worse than the static build — but it is not the
fastest overall (README §6): the range test that finds the hot class
costs about what the arithmetic dispatch costs. The experiment stands
as evidence that runtime specialisation is implementable and safe, not
as the recommended build.
