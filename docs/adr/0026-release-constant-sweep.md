# 0026 — Release-constant sweep: the defaults sit on a flat optimum

*Status: Accepted (2026-11-13). Contemporaneous. The grid was run
with ADR 0027's runtime options, so no rebuilds were involved.*

## Context

ADR 0020 shipped its constants under time pressure — most
visibly the cost-proportional census interval, which was derived
during the jq incident (a −40 % scare traced to censuses walking
a deep, mostly-live stack every ~10 ms) and never swept. Round 5
owes the constants a grid: dead-span age, census interval
divisor, retention quota, and the regrouping fragment threshold,
each against the instruments that can see it — the decay probe
(release latency), the cyclic micros (release tax), the
consolidation probe (ADR 0024), and jq itself (deep stacks).

## Grid and results

**Age (FM_RELEASE_AGE_MS) vs decay-probe landing** (first sample
under 20 MB / final RSS), and vs cyclic micros (ns/op):

| age | landing | final RSS | batch | hold |
|---|---|---|---|---|
| 25 ms | 200 ms | 9,860 KB | 12.17 | 17.08 |
| 50 ms | 200 ms | 9,864 KB | — | — |
| 100 ms (default) | 250 ms | 9,976 KB | 12.20 | 17.36 |
| 200 ms | 350 ms | 9,932 KB | — | — |
| 400 ms | 500 ms | 10,316 KB | 12.24 | 17.15 |
| 800 ms | never (in 500 ms) | 135,648 KB | — | — |

Landing tracks age roughly 1:1 above the ~150 ms mark+detect
pipeline; final RSS is identical for every age that lands.
Cyclic cost is indistinguishable from 25 to 400 ms — the ageing
mechanism, not its constant, is what protects cyclic workloads.
Consprobe settled RSS is flat across age (8.9 MB at 25/100/400).

**Interval divisor (FM_RELEASE_WALK_DIV) vs jq** (v2, REPS=2):

| div | jq time | jq RSS |
|---|---|---|
| 25 (4× rarer) | 3.027 s | 399,392 KB |
| 100 (default) | 3.009 s | 384,776 KB |
| 400 (4× more frequent) | 3.226 s | 375,396 KB |

Rarer buys no time; 4× more frequent costs ~7 % time for ~4 %
RSS. **div = 100 is the knee.**

**Keep quota (FM_RELEASE_KEEP) vs consprobe settled RSS:**
0 → 8,960 KB, 2 (default) → 9,028 KB, 8 → 9,332 KB, 32 →
10,860 KB. Each retained span costs its 64 KB × the classes that
hold one; the default's premium over zero retention is ~68 KB
on the probe while buying the burst absorption ADR 0020 wanted.

**Regroup fragment threshold (FM_REGROUP_MIN, ADR 0024):** lua
peak RSS 19,080 / 19,304 / 19,392 KB at 8 / 64 / 256 and
consprobe flat (8.9 MB) — the default (8) is at least as good as
any alternative on both instruments.

## Decision

**Keep every default.** The sweep's value is negative-space:
the constants derived mid-incident in round 4 were already at
the flat optimum, and the sharp edges are now mapped — age only
moves release *latency*, census frequency trades ~7 % deep-stack
time for ~4 % RSS at 4×, and keep is a pure insurance premium.
Those trade-offs are exactly what ADR 0027's profiles expose:
throughput picks div 25 / age 1000 / keep 8 (pay RSS, buy calm),
footprint picks div 400 / age 25 / keep 0 (pay the jq-style tax,
buy RSS) — with these numbers as their evidence, not folklore.
