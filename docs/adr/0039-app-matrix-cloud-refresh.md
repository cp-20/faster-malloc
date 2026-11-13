# 0039 — Refresh nginx/pgbench/Ruby and demo data on cloud hardware

Status: Measurement accepted; quiet-box ranking deferred (2026-11-13).

## Attempts

Round 6's deferred three cells are run against all seven allocators with
five samples per cell/allocator, rotated/reversed allocator orders. Each
invocation uses REPS=PGREPS=1 to bypass the historical best-run reducer;
we summarize medians, sample SD/CV and ranges externally. nginx keeps the
10 s / 20 connection / 2 worker load; pgbench keeps 15 s / 4 clients /
2 jobs / scale 5, with a fresh database for every cell. Ruby keeps bench.rb.

The cloud container is shared/noisy and has a two-CPU quota. It cannot
satisfy the literal quiet-box condition; report an indicative cloud
re-baseline and leave controlled-hardware rankings open. PostgreSQL here
is v17 rather than historical v16. Other software versions, every raw
sample and medians/variance are saved in `../../bench/results/round8/`.
The Measurements section of `../round8-2026-11-13.md` is the result table.

The harness needed local executable/library overrides and non-root
PostgreSQL support. Server RSS is scoped to the launched master/children,
not all servers owned by a username. nginx temp/log paths live in its
private working directory. These change benchmark portability/measurement,
not allocator behavior or application workload.

One initial nginx/v2 attempt had incomplete RSS output: reading a file
while its sampler truncated/rewrote it could return empty. Publish samples
with atomic rename. Editing the active pgbench script then caused a shell
parse failure after the pgbench workload had reported zero failed
transactions; editing a queued diagnostic script also interrupted it.
The first Ruby invocation also found GNU time absent from the base image;
a locally unpacked time binary plus TIME_BIN override fixed provisioning.
These are operator/harness failures, not allocator failures. Stop on the
failure, freeze scripts, retain the attempts, and rerun affected cells.
`matrix-exclusions.json` identifies exactly those excluded raw records;
no slow sample/outlier is discarded. All other completed cells are retained
on resume. Interrupted diagnostic output is retained separately.

The first final summary encountered zero-mean error/failed-count columns:
CV is undefined there, so record null (with zero SD/variance) rather than
divide by zero. No sample is dropped or changed by this reporting fix.

## Decision

Publish the re-baseline and refresh demo cards with medians, repeat counts,
variance and the shared-container caveat. Keep historical demo charts
labelled as history; do not splice old timings into current data or invent
missing cells. Report ties/unresolved rankings when observed ranges overlap;
this conservative descriptive scoring is not a significance test. Memory,
throughput and latency remain separate metrics. Contention/quota effects
remain a limitation, not evidence to change allocator defaults.

## Consequences

No allocator behavior changes. The quiet-box follow-up remains open.
nginx response framing/body/error checks, pgbench zero-failed-transaction
gates and Ruby's checksum must pass for every accepted sample. The full
round-7 correctness battery and separate FM_DEBUG stress remain required.
