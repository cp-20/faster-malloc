# 0066 — Per-span in-central count with header union overlay

Status: Abandoned before validation (2026-11-16). Implementation reached
correctness under test_basic (after a header-size overflow was fixed with a
large/small union overlay), then reverted unused.

## Attempt

Maintain `_Atomic uint32_t in_central` per span, updated by `fm_publish2`
(+) for span-pure batches and by the refill pop (−), so the census's death
check becomes `in_central + withheld_cnt == nblocks` with no block walk.
The span header is exactly 64 bytes, so the counter was overlaid on the
large-only fields (`user_size`/`align`/`lnext`) via a union — small spans
never carry those and large spans never hold central batches.

## Why abandoned

1. **The proof requires detached-stack-only knowledge.** `in_central` counts
   blocks anywhere on the central stack, including batches published to the
   fresh stack *after* a census detached `all`. A counter-based death proof
   can then unmap a span whose blocks are on the fresh stack (a refill pops
   them into a dangling list). Closing the hole needs a per-span
   publish/pop generation counter plus a two-snapshot check (quiet window),
   and even that admits an ABA publish+pop inside the window unless the
   generation is monotonic — i.e. two atomics per publish and per pop on
   paths that are currently lock-and-remember.
2. **It does not remove the measured cost.** The census walks exist for
   *mixed* batches; the counter covers only span-pure batches, which the
   census already counts wholesale from batch headers. The dominant
   per-block walks (count + rebuild over mixed batches) remain.

## Decision

Keep the header exactly 64 bytes and drop the overlay work. If a future
design needs per-span central bookkeeping, it must (a) prove emptiness from
the detached stack alone and (b) pay for itself by removing mixed-batch
walks — which means the underlying problem is batch composition, not
counting (see ADRs 0062/0063/0068).
