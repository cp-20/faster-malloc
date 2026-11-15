# 0062 — Flush-time span-pure splitting of mixed TLS lists

Status: Rejected for default adoption (2026-11-16). Implemented twice in
`src/fm_core.h` (`fm_flush_bin` mixed path), measured, reverted both times.

## Attempt

Unknown/mixed batches on the central stack are the reason the census counts
and rebuilds walk blocks block by block. `fm_flush_bin` publishes the whole
mixed TLS list as one batch with `count == 0`; a span-pure batch is accounted
and spliced wholesale everywhere. So: at flush time, walk the mixed list once
and split it into per-span chains (local open-addressing table of up to 64
spans; more fall back to one known-count mixed batch), publish each chain as a
span-pure batch, and push the whole split under **one** `fm_stack_exclusive`
region (a first version used per-chain `fm_publish2`, i.e. one mutex
acquisition per chain — visibly slower and abandoned for the batched push).

Combined with the fragmentation regroup prompt (ADR in `ebc8593`), which is
what keeps a split-fed stack regrouped between censuses.

## Measured (best observed form, split + one-lock push + prompt)

- locality: 48.8 ns/op (HEAD 49–61), pages 67.8 preserved, rss 4300 KB (4460)
- trim: after_free ~152000, **after_trim 3032 KB** (HEAD 6768–7068)
- mt 4M xfree: 0.13 s / 2508 KB
- **Ir/op: mixed 100.9 (89.4), retain 106.5 (65.4), cross 78.8 (63.9)** —
  the split's per-block flush walk costs ~6–15 Ir/op in exactly the modes
  whose lists are mixed, and retain/cross/mixed fall behind jemalloc
  (80.1/81.8) and mimalloc (93.1/62.0) on the Ir/op metric that v2 currently
  wins.
- mt 8M and jq wall-clock runs during the same session were load-noisy; the
  deterministic Ir regressions alone fail the no-regression rule.

An earlier measurement of the split under disabled release also showed the
split alone does **not** preserve page concentration (locality pages 512.9
without the census/regroup), i.e. the split is a census-cost optimization, not
a standalone locality mechanism.

## Decision

The flush walk trades Ir/op on retain/cross/mixed for locality/D1/trim —
the exact trade the objective forbids. Reverted. A future attempt needs the
walk to be free (e.g. span-purity maintained by `free` itself with zero
per-block work, or hand-out-order guarantees that make TLS lists naturally
span-clustered), not merely moved from census to flush.

Retained in tree: the fragmentation regroup prompt (`ebc8593`), which the
split motivated; it is independently validated.
