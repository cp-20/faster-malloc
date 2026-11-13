# 0037 — Re-measure wider realloc cache search on real applications

Status: Measurement accepted; default adoption rejected (2026-11-13).

## Attempt and evidence

Retested ADR 0034 without changing allocator sources/defaults. Real lua5.4
5.4.7, Ruby JSON/object churn, Python JSON/large-bytes churn
and jq filter/sort (negative control), both v2 and JIT: nine
alternating off/on pairs after one excluded warmup pair per app/build.
Exact medians, sample variance/CV, RSS, correctness outputs and raw samples
are in the Measurements section of `../round8-2026-11-13.md` and
`../../bench/results/round8/`. This is a shared/noisy cloud container,
re-baselined locally; historical results are not comparison controls.
Separate untimed realloc counters establish workload reach into realloc,
including >32 KiB growth requests. Instrumentation is excluded from timing.

Provisioning tried the documented sudo/helper (unavailable), configured
snapshot (403), then locally unpacked approved Debian packages. Initial
package-name errors were corrected. The first jq warmup corrected a wrong
expected-output literal to the seeded fixture's actual 66395; all saved
outputs agree and the committed driver checks the exact result.

## Decision

**Lua: reject adoption. Ruby: reject adoption. Python: adopt the existing
opt-in for this tested workload, provisionally. jq: reject adoption (control).**
Lua/Ruby/jq have only 5/9 faster pairs per build. Lua medians change by
−0.29% (v2)/−1.11% (JIT). Ruby changes direction: −1.20% v2/+2.90% JIT.
jq's −5.89% v2 median comes with 16.27% on-CV and only −0.33% on JIT.

The realloc counter rejected jq as qualifying coverage: it has zero
non-NULL realloc calls on this build. Keep that attempt as a negative
control and add existing Python churn.py. Its libc path has hundreds of
large-growth realloc requests; Python and Ruby provide the two additional
realloc-heavy applications. Python improves median time **7.88% v2 / 7.70%
JIT**, faster in **7/9 / 9/9 pairs**, and median peak RSS **10.00%** in both
builds with almost no RSS variation. This supports workload-specific opt-in,
not a general claim about every Python program.

Keep `FM_REALLOC_CACHE_SEARCH` **globally default-off**: broader benefit and
controlled-hardware replication remain unestablished. No allocator behavior
is changed by this measurement task.

## Consequences

No allocator behavior change. Python supplies one real-application benefit; global promotion still needs
broader, repeatable evidence beyond shared-host wall-time noise.
