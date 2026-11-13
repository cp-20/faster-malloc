# 0047 — Preserve v1 alignment failure contracts

Status: Accepted (2026-11-13).

## Hypothesis

ADR 0044 left v1 outside its scope. Its `posix_memalign` still clears the
caller's output on invalid alignment and accepts zero. The round7 battery's
v2/JIT loop cannot detect this, and `test_basic` has v2-only cache assertions.

## Audit and change

Checked all five v1 alignment entry points and their shared helper:

- `posix_memalign`: invalid alignment wrote NULL; zero passed validation.
  Reject zero and remove the failure store, exactly as in ADR 0044. ENOMEM
  already returns before the sole success-path output store; retain that order.
- `aligned_alloc`: rejected non-powers of two but accepted zero.
- `memalign`: passed unchecked alignment to the helper, accepting zero and
  non-powers of two. Move power-of-two/nonzero validation into the shared
  `fm_aligned_alloc` helper, returning NULL with EINVAL for both APIs.
- `valloc` and `pvalloc`: use fixed, valid PAGE_SIZE alignment and have no
  caller output parameter. Their failure paths return NULL; no same-class
  defect found and no entry-point change needed.

No allocation policy, size-multiple policy, size arithmetic, or page rounding
changes are included. Valid requests take the existing allocation paths.

## Regression and validation

Add `bench/v1align`, built and cleaned by Makefile and explicitly preloaded
with v1 at the start of `bench/round7.sh`. Volatile indirect calls prevent
compiler builtin assumptions from concealing the output store. Check EINVAL
and an unchanged sentinel for alignments 3, 24 and 0; check unchanged output
on ENOMEM using a huge non-overflowing request. Check NULL for allocation
failure across all five entry points, invalid alignment in both pointer-return
alignment APIs, and valid aligned allocations/free including page APIs.

The new test against the pre-fix v1 DSO aborts at the unchanged-sentinel
assertion for alignment 3. Fixed v1 passes. `make` and the full round7 battery
pass. The round9 verification FM_DEBUG matrix (locked/epoch, static/JIT,
three stress repetitions each, 12 total) and the v1 probe built with FM_DEBUG
also pass. These are correctness checks; no performance claim is made.

## Adopt/reject with reason

Adopt: minimal shared validation and a v1-specific battery entry close the
known contract gap without imposing v2 allocation semantics on v1.
