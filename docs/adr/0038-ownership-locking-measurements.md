# 0038 — Price the central/census ownership bundle

Status: Measurement accepted; reclamation redesign deferred (2026-11-13).

## Attempt and evidence

Same-compiler/O3 round-6 f6d5875 control versus current main 337d7d5 v2.
Nine alternating measured pairs after one warmup pair for eight-thread
local churn, two-thread cross-free, eight-thread many-class transfers,
and the same many-class traffic with concurrent explicit trims.
`bench/ownership.c` uses four independent acquire/release bounded queues,
checks every transfer's endpoint bytes and checks final counts. The legacy
mt cross case has two threads regardless of its final argument.

Full medians, sample SD/CV/ranges, paired outcomes and raw results are in
`../round8-2026-11-13.md` Measurements and `../../bench/results/round8/`.
This is a shared/noisy, two-CPU-quota cloud container, not the reference VM.
The control contains ADR 0035's known races; passing assertions does not
make it safe. No lock was removed from current code. This comparison prices
the ownership bundle, including atomic metadata and census TLS changes;
cold timer-registration/code-layout differences limit lock-only attribution.
An instrumented current build separately counts and clocks mutex acquisition;
its perturbed times are diagnostic, never mixed into throughput results.

## Decision

Retain the correctness fix. The old ~15% cross-free estimate is not a
stable hardware-independent cost: this run gives +45.62% median but large
CVs and only 5/9 slower pairs. Many-class gives +47.73% (7/9 slower);
explicit-census traffic gives +28.81% (9/9 slower, CV 1.62%/6.10%). Local
also gives +10.56%. The first diagnostic run counts 626,404 central acquisitions in
local churn: periodic flush/refill still takes ownership even though the
individual local malloc/free fast paths are lockless. Acquisition-clock
sums overlap between threads and instrumentation perturbs contention, so
they do not isolate all of the wall-time delta. Material contention in the
many-class/census shape merits further optimization work, not a neutrality
claim or a reversion to racy ownership.

Considered epoch/hazard batch reclamation; **defer, no prototype built**.
Header lifetime is only part of the problem. Span census ownership still
needs serialization, and retirement needs thread-exit, stalled-reader and
fork rules. A safe reclamation design and controlled application evidence
are separate work. Disabling locks in the existing implementation would
introduce races/lost ownership and is not a valid performance candidate.

## Consequences

No allocator behavior/default changes and no unsafe lock-elision experiment.
Keep the reproducible many-class harness to price a future safe design.
