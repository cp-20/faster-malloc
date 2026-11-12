# 0009 — Measurement methodology: instruction counts first, wall clock on real programs

*Status: Accepted. Provenance: Reconstructed (2026-11-12, from README
§4 and the `bench/*.sh` harness).*

## Context

The development host is a shared, heavily virtualised WSL2 box whose
wall clock has a ~3.5 % noise floor and 5× outliers (a neighbour
process at 164 % CPU; the same program measured 5× apart across runs).
Wall-clock microbenchmarks on such a host cannot separate allocators
that differ by a few percent.

## Decision

Layered evidence, in order:

1. **Instruction counts (callgrind)** are the primary metric — exact
   and reproducible. The harness measures the delta between two
   iteration counts so dynamic-loader startup cancels out.
2. **Cachegrind** (D1/LL misses, branch mispredicts) explains *why*.
3. **Wall clock on real programs** (cfrac, espresso, barnes) is the
   final word: round-robin order, minimum of several runs, peak RSS
   reported alongside.
4. `bench/selfcheck.sh` quantifies the harness's own error floor by
   benchmarking the same allocator under two labels (3.5 % with
   min-of-15).

## Consequences

Claims in the README rest on instruction counts that reproduce
bit-for-bit; the wall-clock table moved up to 10 % between runs while
the counts did not move at all. New benchmarks must fit this harness:
deterministic, one comparable number, and a self-checkable noise
floor. The 2026-11-12 macro benchmarks (see the work log) follow the
same discipline with interleaved min-of-N wall clock where callgrind
is unavailable.
