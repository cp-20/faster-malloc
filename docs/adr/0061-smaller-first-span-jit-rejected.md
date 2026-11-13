# 0061 — Smaller first span in JIT: rejected

Status: Rejected (2026-11-13); JIT retains 64 KiB first spans.

## Attempt

Apply ADR 0060's 16 KiB first-span policy in the shared core to both static and
JIT. Both builds pass the full release/FM_DEBUG batteries and six intensified
stress runs; both show non-overlapping startup-HWM improvements. The initial
shared-policy data and verification are retained in
`bench/results/round12-startup/first16-*` and `shared-first16-verification/`.

## Guard and decision

Seven alternating app-matrix/KV repetitions were too noisy to accept on median
wall time alone. Run nine more pairs with per-target wall and CPU time. Keep
all observations; no slow samples are excluded. Each pass has overlapping
ranges and the second pass's confidence intervals include zero, but pooling
the paired **wall-time ratios** across both passes reveals a small JIT KV
signal: median +3.93%, percentile bootstrap 95% interval +0.23% to +12.17%
(10,000 draws, seed 12). The timers differ by driver overhead, so this pooled
comparison normalizes each candidate to its adjacent baseline within its own
pass. It does not treat raw durations from different timer paths as identical.

That signal fails this round's conservative no-regression adoption rule. It
might be address-layout/cache interaction rather than instruction overhead;
the experiment does not establish a mechanism. Do not dismiss it just because
individual min/max ranges overlap. Reject the JIT application of the policy;
retain JIT's full first span with no added atomic selection. Record static's
separate result in ADR 0060. The common actual-map-length mincore query remains
safe for both builds; for JIT its argument is still 64 KiB.

This is not a claim that JIT can never use smaller first spans. A future attempt
needs a different policy or quiet-host evidence that resolves the churn signal.
No deferred-JIT-metadata change was attempted: its measured startup contribution
is only tens of KiB.
