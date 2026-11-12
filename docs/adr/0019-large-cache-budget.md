# 0019 — Large-block cache budget: 4 GiB → 64 MiB

*Status: Accepted (2026-11-12). Recorded contemporaneously.*

## Context

ADR 0011 introduced the per-thread large-block cache with a byte
budget of 4 GiB — a *virtual* figure chosen when the concern was
never evicting under churn. Round 3's app matrix showed what that
costs in resident terms: cached blocks keep their touched pages, so
the budget is effectively "retain up to 4 GiB of RSS per thread".
v2's peak RSS in the baseline matrix was the worst or near-worst
of seven allocators in six of eight real apps (Python +28%,
Node +25%, Lua +66%, SQLite +62% vs glibc), while its times were
already competitive — retention, not speed, was the gap.

## Sweep

Same workloads, five budgets, interleaved min-of-3 (times in
seconds, peak RSS in MB):

| workload | 4 GiB | 256 MiB | 128 MiB | 64 MiB | 32 MiB | 0 (off) |
|---|---:|---:|---:|---:|---:|---:|
| Python churn RSS | 551 | 551 | 525 | 486 | 461 | 434 |
| Python churn time | 4.96 | 4.74 | 4.52 | 4.60 | 4.78 | 5.01 |
| Node churn RSS | 528 | 530 | 528 | 499 | 471 | 449 |
| Node churn time | 1.92 | 1.70 | 1.73 | 1.75 | 1.75 | 1.77 |
| Lua churn RSS | 19 | 19 | — | — | 19 | 16 |
| micro large 64 KiB | 0.006 | 0.006 | — | — | 0.006 | **2.97** |
| micro pareto | 0.024 | 0.024 | 0.119 | 0.365 | 0.481 | 0.605 |
| rgrow ladder | 0.29 | — | 0.21 | 0.20 | **1.84** | — |

(glibc references from the same round of testing: Python RSS
440 MB, Node RSS 433 MB, pareto ≈ 0.31 s.)

Reading the table:

- The cache itself is not negotiable: disabling it costs 500× on
  single-size large churn (every allocation becomes
  mmap + faults + munmap). ADR 0011 stands.
- Python/Node RSS falls roughly linearly as the budget shrinks —
  most of v2's excess footprint over glibc *is* the large cache
  (at budget 0, Python is at glibc parity; the rest is spans and
  local lists). Times barely move until the budget reaches 0.
- Two synthetic workloads set a floor. The rgrow ladder
  (ADR 0018) keeps ≈64 MiB of freed rungs in play; a 32 MiB budget
  evicts them mid-ladder and costs 9×. The pareto scenario
  (1,024 live blocks, wide size spread) has a free-block working
  set above 128 MiB and only reaches full speed at 256 MiB.

## Decision

Budget = **64 MiB** per thread. It is the smallest budget at which
every measured workload except pareto runs at full speed
(rgrow 0.20 s — the ladder fits), it cuts the Python/Node peak-RSS
excess by roughly half (551→486 MB, 528→499 MB), and pareto lands
at ~glibc parity (~0.37 s vs ~0.31 s) rather than multiples of it.
Pareto's full-speed point (256 MiB) was rejected deliberately: it
buys a synthetic capacity probe its headline number and gives up
nearly all the real-app footprint win. A workload that genuinely
churns >64 MiB of large blocks per thread will pay mmap/fault
costs the old build avoided; that is the accepted price, and
ADR 0018 documents the related trap (policies that bypass the
cache lose far more than they save).

Combined with ADR 0017 (thread-exit drain), per-thread retention
is now bounded in both dimensions: at most 64 MiB of large cache
while the thread lives, and none of it survives the thread.
