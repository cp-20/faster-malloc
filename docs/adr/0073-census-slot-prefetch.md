# 0073 — Prefetch census slots in the census/rebuild walks

Status: Accepted (2026-11-16, commit 51936cf).

## Change

The census's count and rebuild passes chase serial block chains and probe
the census table per block; each step stalls on the link-word load and then
on the slot's first touch. The slot index is computable from the link word
alone (span = block & SPAN_MASK, slot = span >> 16), so the NEXT block's
slot is prefetched one step ahead, overlapping the two misses.

## Results

cachegrind D1mr+D1mw, jq A: 8,417,954 → 5,796,945 (−31%); locality and loc3
unchanged (their stacks hold mostly span-pure batches). Ir/op within +0.5
(retain 65.4 → 65.9, cross 63.9 → 64.3; churn/churnmix/mixed identical).
v2/jit test_basic+stress pass, loc3 67/68 pages, trim unchanged.

Related rejection: the broader five-site prefetch (ADR 0070) gained nothing
because it prefetched block links the walk must load anyway; the win came
from prefetching the *slot*, whose index is available early.
