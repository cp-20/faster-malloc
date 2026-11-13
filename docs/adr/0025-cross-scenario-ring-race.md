# 0025 — bench/micro `cross` measured its own race; the ring is now bounded

*Status: Accepted (2026-11-13). Harness fix, recorded
contemporaneously.*

## Context

Round 4 documented a pre-existing defect in `bench/micro`'s
`cross` scenario (one thread allocates, another frees — the
Larson-style cross-thread-free measurement) and left it unfixed:
the scenario aborted with a tcache double-free under *stock
glibc*. Round 5 needs cross-thread throughput as a standing
measurement (it is one of the shapes real servers exercise), so
the harness bug had to die first.

## The two defects

1. **The producer could lap the consumer.** The ring used
   monotonic head/tail counters with capacity `cap`, but the
   producer never checked `head - tail < cap`. When it wrapped
   past the consumer it overwrote slots still holding live
   pointers; the consumer later freed some blocks twice (and
   leaked others). The allocator under test was irrelevant — the
   scenario corrupted its own queue.
2. **It never terminated on its own.** `main` joined the
   producer *before* setting the stop flag the producer loops on,
   so a run ended only via the double-free abort or malloc
   returning NULL at OOM. The `rounds` argument was ignored.

## Fix

- The producer emits exactly `cap * rounds` blocks and waits
  (pause/yield spin) while the ring is full; the consumer drains
  until the producer's done flag is set and the ring is empty.
- `sc_cross` verifies `freed == produced` and fails the scenario
  otherwise, so a future queue bug reports as a harness failure,
  not as allocator data.
- `bench/run.sh` integration: `cross` now parses (it reports
  ops/sec; the runner converts), runs unpinned (pinning a
  two-thread scenario to one CPU measures the scheduler, not the
  allocator — under `taskset -c 3` every allocator scored an
  identical ~1.39 M ops/s), and reports the RSS of the same run
  that produced the best time (previously every row printed the
  last run's RSS). v2 (`fastmalloc2`) joins the runner's
  allocator set.

## Evidence

`micro cross 64 4096 256` (1,048,576 blocks through a 4096 ring),
direct runs, all four in-tree allocators terminate cleanly with
`freed == produced`:

| allocator | ops/s |
|---|---|
| glibc | 6.0 M |
| v1 | 12.8 M |
| v2 | 12.2 M |
| jit | 8.7 M |

Standing suite (`bench/run.sh cross`, REPS=3, min): glibc
5.6 M, tcmalloc 3.3 M, jemalloc 5.7 M, mimalloc 6.1 M, v1
13.6 M, jit 7.0 M ops/s. Cross-thread free throughput is a
measurable, standing row again.

## Decision

**Accepted.** The scenario's semantics are now what its name
always claimed; numbers from any earlier `cross` run (there were
none in any round doc — round 4 found it broken before use) are
void by construction.
