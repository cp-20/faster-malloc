# 0044 — Preserve posix_memalign output on invalid alignment

Status: Accepted (2026-11-13).

## Hypothesis

The namespaced ASan battery fails `test_basic` at `posix_memalign align=3`
for v2: the implementation clears `*out` before returning EINVAL. POSIX
requires leaving the output unchanged on failure. The identical JIT entry
point has the same defect. Zero alignment also passes the old validation.
GCC recognizes the ordinary function name and can assume the output is
unchanged on an error, concealing the implementation defect in the existing
direct-call test.

## Change

Reject zero alignment and remove the error-path output store in v2 and JIT.
Call through a volatile function pointer in `test_basic` and check both EINVAL
and an unchanged sentinel for alignments 0, 3 and 24. Successful allocation and
ENOMEM already store the output only after success. V1 is outside this change.

## Measured numbers

Before: all four namespaced v2 ASan basic cases (default, balanced, throughput,
footprint) exit 1 at alignment 3. After: the same four cases exit 0, with the
three invalid alignments checked through the indirect call. Complete sanitizer
matrix and reproduction commands are in the round-9 “Sanitizer proof” section.
No performance conclusion is drawn from instrumented runs.

## Adopt/reject with reason

Adopt. This fixes a confirmed API contract violation with a regression that
cannot be optimized away using the compiler's knowledge of posix_memalign.
It changes only invalid-alignment handling; no allocation policy changes.
