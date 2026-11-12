# 0004 — Parallel TLS arrays and frameless hot paths

*Status: Accepted. Provenance: Reconstructed (2026-11-12, from README
§2.3–§2.4 and `src/fm_core.h`).*

## Context

Per-thread free-list state was originally an array of structs
(head/tail/count per class). With a 32-byte struct stride the compiler
emits a shift and an add on the critical path, and the layout forced the
slow paths to be inlined, so `malloc` grew a stack frame and six
callee-saved register spills.

## Decision

Split the thread-local state into three parallel arrays
(`fm_head`, `fm_tail`, `fm_cnt`), so the 8-byte stride of `fm_head`
folds the class index into a single instruction's addressing mode, and
mark every slow path `noinline` so `malloc`/`free` need no stack frame
and no callee-saved registers.

## Consequences

Recorded as worth 7× on its own: the churn loop went from a measured
144 ns/op to 22 ns/op on the development host, and instruction counts
confirm the direction (the whole malloc fast path is 13 instructions).
The `fm_cnt` counter counts frees since the last refill/flush rather
than the exact list length, which is what makes the flush test a single
compare.
