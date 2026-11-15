# 0072 — Batch-header pool leak fix and fragmentation regroup prompt

Status: Accepted (2026-11-15, commit ebc8593).

## Change 1 — pool leak

`fm_batch_alloc`'s fresh 128-header page returned `first[0]` and set
`fm_batch_pool = &first[127]`, whose `next` is NULL: 126 of 128 headers were
orphaned (leaked) per page once recycling was exhausted — any workload that
outruns the pool burned a fresh page per allocation (measured ~8 MB retained
RSS in a 64 B-class churn loop). Fix: `fm_batch_pool = &first[1]`.

## Change 2 — fragmentation prompt

The regroup prompt fired only while the stack head was uncharacterised
(count 0). A stack of many small *characterised* batches — the shape
flush-time churn leaves — never prompted, so refills popped tiny batches one
at a time. The prompt now also fires when the head batch is characterised
but small (< 32 blocks) while depth ≥ 64: a regrouped stack's head is always
its largest chain, so a small head is the fragmentation signature. Rate
limit unchanged (1-in-128 flushes).

## Results

malloc_trim after_trim 6768–7068 → 3012–3016 KB (glibc 2644; mimalloc/
jemalloc/tcmalloc 177040/156064/173332), trim after_free ~161400 → 143–151
KKB-band, mt 8M xfree RSS ~3100 → 2784–2808 KB, locality RSS 4460 → 4176 KB
with pages unchanged (67.8), D1 locality 2225007 → 2217534 and loc3
1127324 → 1121374, Ir/op identical, jq parity with peak RSS down ~2 MB,
v2/jit test_basic+stress pass.
