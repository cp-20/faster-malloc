# 0047 — Python and Node peak RSS attribution

Status: Accepted measurement (2026-11-13).

## Hypothesis and method

The 64 MiB per-thread large cache, rather than dead-span ageing alone, explains
most of v2's excess peak RSS. Measure glibc, jemalloc 5.3.0, mimalloc 2.1.7 and v2
in this same two-CPU-quota container, using the existing app fixture/workloads and
`run_timed.py`. Five rotated rounds; report medians and min–max, never best runs.
Python is 3.12.14; Node is v24.22.1; compiler is GCC 14.2.0.

A separate generated diagnostic v2 counts live usable capacity and marks cached
large mappings. A parent pauses all threads, identifies resident span headers,
reads pagemap residency, and retains the largest RSS snapshot at roughly 50 ms
intervals, three runs per app/configuration. Fully free small spans are separate
from partially live spans. Baseline startup HWM is measured independently;
`other` is the residual, including runtime-direct mappings, code and stacks.
See `bench/round10-shortlived/README.md` for attribution limits and reproduction.

## Measurements and decision

Numbers, ranges and raw-file links are in the short-lived section of
`docs/round10-2026-11-13.md`. Accept attribution as a sampled diagnostic, not an
exact accounting of historical HWM or requested live bytes. Do not subtract
usable capacity from RSS and call the difference exact fragmentation. Cached
mapping residency, rather than the configured virtual-byte budget, is the
relevant quantity.

The exploratory matrix overlapped diagnostics/build work; retain it as audit
material and exclude it from final medians. Final matrix, diagnostics, correctness
battery and probes run serially. Lua is excluded per ADR 0031.

Three diagnostic runs each, median (min–max), resident KiB:

| App/profile | Total sampled RSS | Cached large | Fully free small spans |
|---|---:|---:|---:|
| python/v2 | 502,904 (502,904–502,904) | 60,632 (60,632–60,632) | 1,556 (1,556–1,556) |
| python/shortlived | 441,892 (441,892–441,896) | 0 (0–0) | 1,556 (1,556–1,556) |
| node/v2 | 496,204 (490,924–497,052) | 51,276 (51,276–51,276) | 6,332 (6,184–7,296) |
| node/shortlived | 443,476 (440,412–445,836) | 0 (0–0) | 6,992 (6,504–7,080) |

The synchronized live-large, partially live span, usable-capacity, baseline
and anonymous-residual columns are in the round-10 report. Zero-cache retains
no cached-large pages while the Python live-capacity and anonymous residual
are essentially unchanged. This supports the cache-retention hypothesis.
