# 0065 — Free/refill-side span-purity and ordering probes

Status: All rejected (2026-11-13 … 2026-11-16). Each implemented, measured,
reverted. Recorded together because they share one hypothesis: keeping TLS
free lists span-pure or address-ordered at the source improves central
regrouping, reuse placement, and cache behavior without a census walk.

## Boundary flush on span change (`fm_free`)

When freeing into a non-empty known-pure list whose span differs from
`fm_list_span[cls]`, flush the current bin before pushing. Tried three times
(`d38ea0c` era, post-`667cf2e`, post-`ebc8593`): correctness/pages/Ir always
preserved, but no simultaneous improvement in mt RSS, locality ns/D1, trim,
or macros (e.g. 0.14 s/6020–6456 KB mt 4M, locality 51.3–65.5 ns/67.8 pages,
D1 2,224,869–2,224,874). Also measured with release disabled: pages still
collapse to ~513 without the census, i.e. source purity is not sufficient by
itself.

## Refill: unknown-batch classify-on-refill

When popping an unknown-count batch in refill, walk it once to recover
count/span-purity. **Result:** mt 4M 0.16 s/19840 KB (severe), locality
77.3 ns, Ir mixed/retain/cross 91.3/81.5/67.2 — the walk runs on the refill
slow path but the count/purity results also changed flush behavior; net
regression. Reverted.

## Refill: span-group reorder of unknown batches (2-pass grouping ≤16 spans)

Group an unknown batch's blocks by span (two passes over ≤16 groups) and
concatenate per-span runs before installing. **Result:** mixed/retain/cross
Ir 101.0/101.3/81.4, locality 52.1 ns but mt 0.15 s/6148 KB and macros worse;
the per-refill reordering cost lands in exactly the churny workloads.
Reverted.

## Refill: address-sort on refill

Collect a popped batch into an array and insertion-sort by address so
allocation order is ascending. **Fatal defect:** batches can exceed the
256-entry collection array; truncation silently leaked the tail of the
chain (loc3 pages 133–135, mt 0.69 s/28984 KB, Ir/D1/trim all worse).
Reverted for correctness as well as performance. A correct version needs
heap allocation for the sort or an in-place merge, i.e. real cost.

## Refill: publish bump remainder on every refill-from-bump

Publish the existing bump range's remainder as a span-pure batch each time a
refill carves from it (unifying with the fresh-span path so all span blocks
are census-visible). **Result:** Ir unchanged, mt 0.13 s/6532 KB, locality
47.6 ns, trim 161424/7024 KB — no cross-metric win. Reverted (the code
simplification was tempting but the objective requires all-metric wins).

## Common decision

None of these probes produced an all-metric win; several regressed Ir/op in
retain/cross/mixed, which v2 currently wins. The retained insight: TLS lists
become span-mixed in exactly the workloads that matter, and *any* per-block
source-side fix shows up in Ir/op there. A zero-cost purity source (hand-out
order guarantees — see ADR 0068) is the only shape that could work.
