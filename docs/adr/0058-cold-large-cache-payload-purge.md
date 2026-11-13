# 0058 — Discard payload in large-cache buckets without demonstrated reuse

Status: Rejected for production adoption (2026-11-13); prototype retained as a patch.

## Hypothesis and mechanism

On main 3d1e114, Lua's sampled peak includes 3,140 KiB of cached large
mappings, larger than fully-free small-span residency (2,292–2,540 KiB).
Partially-live spans occupy 6,884–7,128 KiB, but include most of the
5,894 KiB live small usable capacity. Merely labeling all partial-span
residency returnable overstates that target. The cache is the largest
unambiguously returnable measured component. The old ADR 0031 proportions
are not assumed to hold on this build/runtime.

One prototype: `FM_LCACHE_COLD_PURGE=1` discards whole payload pages on cache
admission until that per-thread logarithmic bucket has served its first cache
hit. Keep the header page and mapping, cache search, limits and accounting.
A successful hit permanently warms that bucket for the thread's remaining
life, even across trim. This is deliberately conservative: no timers, miss
count thresholds or cross-thread history. The next hit may incur zero-fill
faults; subsequent reuse keeps residency. Failed madvise simply retains
pages. The prototype opt-in defaults off, independent of FM_PROFILE.
It is not a shipped option.

## Correctness and scope

Only plain freed large blocks admitted to the calling thread's TLS cache
are advised, from the first full page after the header through mapping end.
There are no valid payload readers after free. Cache links and span metadata
remain resident. Aligned/uncacheable blocks keep their existing unmap path.
No live allocation is advised. Realloc's existing copy precedes freeing its
old mapping. A cache hit still reports non-zeroed storage, because the
header page's payload is retained; calloc continues clearing the full request.
The 48 buckets fit the 64-bit TLS mask; history is advisory and thread-owned.
Fork inherits conservative history without a new lock. Thread teardown needs
no extra action. All ADR 0035 census/central mutexes, atomic dead_mark,
intersect/start counts, withheld geometry and reader lifetimes are unchanged.

The focused FM_DEBUG test checks resident cold payload becomes nonresident,
header validity, identical-address reuse, warm retention, drain and calloc
contents. Full round7 validation covers both static/JIT with off/on and
ordinary/debug configurations. This is not an exhaustive race proof.

## Measurements and decision

See `docs/round12-2026-11-13.md` and `bench/results/round12` for raw samples,
medians, full ranges and verification logs. No other mechanism was prototyped
in this task. The first five Lua pairs improve peak RSS from 17,896
(17,800–17,960) to 14,844 (14,772–15,220) KiB, with non-overlapping ranges.
However, many-class churn regresses from 1.019 (0.858–1.068) to
1.238 (1.039–1.441) seconds, slower in all five measured pairs. The
predeclared ten-round follow-up retains that direction: baseline 1.072
(0.796–1.930), identical baseline repeat 1.179 (0.944–1.398), candidate
1.259 (0.978–2.125), candidate option off 1.180 (0.964–1.910) seconds.
Shared-container variance is substantial, but neither overlapping ranges nor
an explanation based on code layout is enough to claim no regression.

Reject production adoption: the Lua peak gate passes, but guard neutrality
is not established. All four prototype correctness batteries pass; correctness
does not waive the performance gate. The 15-line allocator diff is preserved
in `bench/round12/cold-purge.patch`, with its focused test and reproduction
drivers. Production allocator sources and configuration remain at main
3d1e114. No new runtime option or profile ships. No correctness property is
relaxed to rescue the result.

Final unchanged production sources also passed full ordinary and FM_DEBUG
round7 batteries; the restored DSO matches the measured main control exactly.
