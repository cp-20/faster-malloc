# Parallel allocator experiments (exp/*)

This directory documents the screening loop used to improve `src/fm_core.h`
without running heavy benchmarks on the development laptop.

## Goal

Make `libfastmalloc2.so` (v2) the fastest allocator on the repository's
benchmark suite.  Two signals, in priority order:

1. **Exact instruction counts** from callgrind on the `bench/instr`
   workloads.  These reproduce bit-for-bit and are the primary gate.
2. **Wall clock and peak RSS** on the real-application fixtures in
   `bench/apps/real_suite.py`, measured interleaved against the merge-base
   in the same job.  This is the "fastest on real programs" signal; it is
   noisier, so a candidate needs a clear win, not a 0.2% blip.

## The loop

1. Branch from `main` as `exp/<short-name>`.
2. Keep the change small and local to `src/fm_core.h` (or
   `src/fm_static.c` when the change is entry-point-only).
3. Compile and run correctness locally:
   ```
   make -j"$(nproc)" libfastmalloc2.so libjitmalloc.so
   LD_PRELOAD=$PWD/libfastmalloc2.so ./test_basic
   LD_PRELOAD=$PWD/libfastmalloc2.so ./bench/stress
   ```
   Do **not** run callgrind, cachegrind, `bench/suite.sh`, `real_suite.py`,
   `ab.py`, or any other benchmark locally.  The laptop is shared and the
   CI runner is the measurement instrument.
4. Commit, then push the branch:
   ```
   git push origin HEAD:refs/heads/exp/<short-name>
   ```
   The push triggers `.github/workflows/exp-bench.yml`, which builds the
   candidate and its merge-base, runs the correctness gates, and measures
   both.
5. Read `exp-results.json` from the run artifact (or the job summary) and
   decide.  A candidate is interesting when `score.micro_geomean < 1.0`
   and `score.real_geomean <= 1.0`, with no single real app above ~1.03.

Pushing many `exp/*` branches at once is expected: the repo is public and
the screening jobs run in parallel.

## Rules

* One hypothesis per branch.  Do not bundle unrelated changes.
* No benchmark harness changes, no workflow changes, no changes to the
  benchmark fixtures.  Those invalidate comparisons.
* No new runtime defaults unless the CI result supports them.  Prefer a
  compile-time constant or a change in the existing slow path.
* Preserve the public ABI: `malloc/free/calloc/realloc`, aligned entry
  points, C23 sized free, C++ `operator new/delete`, `malloc_trim`.
* `FM_DEBUG` and the sanitizer builds must still compile.
* If the experiment is not a clear win, say so and leave the branch
  unmerged.  A measured "no" is useful.

## Already measured and rejected

Read `docs/adr/README.md` before changing an area.  In particular, these
ideas were already tried and rejected, so do not spend a branch on them
unless the new mechanism is materially different:

* free-list shaping / span-pure splitting on the free or refill side
  (ADR 0014, 0021, 0062, 0065)
* mremap-based realloc growth and large-mapping extension (0018, 0033)
* central-lock striping as a default (0048)
* bounded census spin, busy hints, per-thread census backoff (0055–0057)
* cold large-cache payload purge (0058) and smaller first spans in the JIT
  (0061)
* JIT jump-table dispatch (0022)
* prefetch/config micro-probes (0070)
* immediate/zero-age span release (0020, 0053)

## Baseline evidence

`docs/results.md` and `docs/bench-dashboard/history.json` hold the historical
numbers.  ADR 0078 holds the most recent fast-path and syscall work.  Always
compare against the merge-base measured in the same CI job, not against an
old document.
