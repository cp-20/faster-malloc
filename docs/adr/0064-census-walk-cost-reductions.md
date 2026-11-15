# 0064 — Census walk-cost reductions: histogram batching, qsort skip

Status: Both rejected (2026-11-16). Implemented in `src/fm_core.h`, measured,
reverted.

## Per-span histogram before `fm_census_add`

Mixed batches touch few distinct spans in many workloads, so count each
batch's span histogram in a 16-entry local array and add once per distinct
span (one hash probe per span instead of per block); overflow falls back to
per-block adds.

**Result:** locality callgrind Ir 103.05M total (HEAD 70.11M), D1 2,328,199
(2,225,007), locality 67.1 ns. Mixed batches in the hot workloads have more
than 16 distinct spans (jq/locality churn scrambles addresses), so the
histogram overflows and the batch is walked twice. Reverted. A 64-entry
table would add stack pressure without removing the second walk in the
overflow case.

## Qsort skip for the regroup-only pass

`fm_trim_class` sorts the rebuilt span chains largest-first so refills pop
the fullest spans first. For the depth-adaptive regroup-only pass, skip the
order array + qsort and push batches in census-iteration order.

**Result:** locality 91.0 ns (49–57), D1 2,226,099, jq 0.31 s. The spill
order is load-bearing for refill locality even on shallow stacks. Reverted.
Note: profiling attributed the earlier "qsort 10.7%" to the *benchmark's own*
address sort; the allocator's per-span qsort is over ~33 entries and is not
a real cost — do not re-litigate this without a measurement showing
`msort_with_tmp` inside `fm_trim_class` itself.
