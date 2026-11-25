# Optimization goal

Make `libfastmalloc2.so` (v2) **first in both wall time and peak RSS on
every application in the benchmark set**.  Being fastest on time while
using more memory is not done; being smallest while slower is not done.

The set has two parts:

* the seven `exp-matrix` apps: g++ compile, jq, sqlite OLTP, Python data,
  git revlist, xz and sort;
* the dashboard's legacy matrix (rounds 3-6): redis, sqlite speedtest1,
  legacy jq, Lua, CPython, Node, gcc and git, re-baselined by
  `exp-legacy.yml`.

This file is the standing brief for the recurring optimization campaign.
Every campaign cycle starts by reading it and `docs/exp/backlog.md`.

## Scoreboard

Two signals, both first-class:

1. **Exact micro instruction counts** from `bench/ci/expbench.py`:
   `score.micro_geomean`, `score.micro_worst`.  Reproducible bit-for-bit.
2. **Real applications** from the same job:
   `score.real_paired_geomean`, `score.real_paired_worst`, per-app
   `paired_ratio`, and peak RSS.  `real_geomean` (ratio of medians) is a
   secondary view; short apps such as `sort` are noisy, so prefer the
   paired statistic and the raw times.

The public claim is the full matrix (v2 vs glibc, mimalloc, jemalloc,
tcmalloc), not the ratio against main alone.  For every app the cell is
green only when v2's median time and peak RSS are both <= the best
competitor (ties inside the sample range are allowed).

## Current standing (2026-11-24, 7 reps)

Time: v2 is first on 5/7 (g++ compile, jq, Python, xz, sort), 0.5% behind
on sqlite and 1 ms behind glibc on the 17 ms git revlist.

RSS: v2 is **not first in any of the seven**.  The gaps to the best
competitor are:

| app | v2 RSS | best competitor | gap |
|---|---:|---:|---:|
| g++ compile | 200.6 MiB | glibc 193.9 | +3.5% |
| jq | 262.6 MiB | glibc 262.5 | +0.04% |
| sqlite OLTP | 13.4 MiB | glibc 11.4 | +17.5% |
| Python data | 146.1 MiB | glibc 141.6 | +3.2% |
| git revlist | 13.2 MiB | glibc 11.4 | +15.8% |
| xz | 56.3 MiB | glibc 56.0 | +0.5% |
| sort | 45.7 MiB | glibc 32.9 | +38.9% |

The legacy dashboard rows are stale (rounds 5/6/10).  `exp-legacy.yml`
must be run on current main before claiming anything there; its results
become part of the same scoreboard.

Update after the first RSS wave: a cold large-cache purge in the balanced
defaults (`exp/rss-balanced2`) measured **sort 45 -> 35 MiB** and jq
262 -> 257 MiB (v2 first), with jq +1.8% / Python +1.4% time.  A size-band
threshold is in flight to keep the sort win without the medium-buffer
time cost.  Shrinking the first spans made git/sqlite worse; the startup
gap needs a different mechanism (fewer mappings, not smaller ones).  The
Redis 8 / jemalloc `malloc_usable_size` crash is fixed (PR #2).

## Frontier after PR #5 / PR #6 (2026-11-25)

Merged: ADR 0083 (16 MiB base budget), 0087 (selective warm/cold retention,
runtime knobs), 0090 (dense-THP warmup knob).  11-rep legacy, MiB and
time ratio against glibc:

| app | v2 RSS | glibc | ratio | v2 time / glibc |
|---|---:|---:|---:|---:|
| jq | 353.9 | 370.4 | **0.955** | **0.740** |
| python | 432.0 | 430.1 | 1.004 | 1.010 |
| node | 454.6 | 438.3 | 1.037 | 1.083 |
| gcc | 358.2 | 342.3 | 1.046 | 0.989 |
| lua | 14.7 | 11.6 | 1.260 | 0.716 |
| git | 28.2 | 22.1 | 1.276 | 0.980 |
| sqlite | 13.7 | 11.7 | 1.170 | 0.999 |
| redis | 6.3 | 5.6 | 1.131 | 0.897 |

Every residual has now been decomposed and bounded by measurement:

* **32 KiB class (gcc's dominant shape)**: a 32768-byte payload plus the
  64-byte header cannot fit eight pages, and a 64 KiB span holds one
  block, so each live block touches nine pages: 38,848 B of RSS against
  glibc's 34,752 (+11.8%, measured with `/proc/self/statm`).  A 32704
  class fixes it only for requests <= 32704; the CI toolchain allocates
  exact 32 KiB (Round 39).  Out-of-band headers need a page map: 16
  `& SPAN_MASK` sites and a lookup on every free, whose cost proxy (a
  two-compare range check, 7 of 44 instructions) measured micro
  1.12-1.16.
* **Warm 64/128 KiB buckets (gcc, sqlite)**: the cache serves 22,459 and
  12,341 hits in one gcc run, so its retention is buying time; cutting it
  is the measured RSS-for-time trade (ADR 0083 A/B: 4.1 points of RSS for
  2.9 points of time).
* **git / lua / sqlite / redis small-process gap**: fully-free pages are
  only 58 pages (232 KiB), census frequency and refill order are no-ops,
  and byte-granular packing costs the micro 1.12-1.16.
* **node**: 27 MiB payload retention is the same trade; the knobs expose
  both ends (`FM_LCACHE_MAX_BYTES=67108864` restores time at +60 MiB).

### Process correction: the stale baseline

Several candidates in Rounds 35-40 appeared to win ~18 MiB on gcc locally
and then changed nothing on the CI matrix.  The cause was found: the
local worktree was still checked out at the pre-merge commit (ac6b631),
so every local A/B measured the candidate against a *pre-PR#5* library.
The apparent 18 MiB was the selective cache's own effect, already in
main.  Re-measured against `origin/main` (920463f) on the same fixtures:

| build | gcc RSS | lua RSS | sqlite RSS |
|---|---:|---:|---:|
| main (920463f) | 351.5-351.8 MB | 14.6-15.0 MB | 11.14-11.17 MB |
| 32704 class | 351.5-351.8 | 14.8-15.0 | 10.90-11.13 |
| cold purge on insert | 351.3-351.8 | 14.8-16.3 | 11.05-11.10 |
| node, 2 MiB THP hint | 351.6-351.7 | - | - |

All three are no-ops, exactly as the CI reported.  **Rule for future
cycles: rebuild and assert the baseline from `origin/main` HEAD before
any local A/B.**

The standing candidate is therefore at the measured Pareto frontier of
this architecture; the remaining options are explicit knobs
(`FM_LCACHE_BUCKET_COLD=0`: sqlite 11.1 -> 9.4 MiB at +15% time;
`FM_DENSE_THP_WARMUP=0`: git -1.9 / lua -2.0 MiB at jq +7.9%).

## Baseline

The campaign starts from whatever is on `main`.  Accepted mechanisms so
far (see ADRs 0079-0082):

* full 64 KiB per-class local byte budget;
* one-request headroom for geometric large realloc growth;
* adaptive warm large-cache budget (2x, capped at 128 MiB);
* 4 MiB per-thread large slab.

`main` is the merge base; every experiment is screened against it in the
same CI job.

**Do not open a PR for `exp/combined-v2` or for anything already marked
ACCEPTED in the backlog.**  That batch was merged as PR #1.  A campaign
cycle opens a PR only for a *new* branch created by that cycle that beats
the current `main` on both time and RSS.

## A campaign cycle

The scheduled run is a manager, not a lone coder.  It should:

1. Read `docs/exp/backlog.md`, the latest `collect.py --all` leaderboard,
   and `git log --oneline -10` on main.
2. Count in-progress `exp-bench` runs.  If more than 12 are in flight,
   report and stop the cycle (the previous fleet has not drained).
3. Pick **at most 6** hypotheses:
   * first, follow-ups that tune a branch whose result was close to a win
     (`micro_geomean < 0.995` or `real_paired_geomean < 0.99`, but not
     adopted);
   * then the first unclaimed TODO items in the backlog.
   Avoid a hypothesis if a branch with the same name already exists or a
   run for it is in flight.
4. Launch one `delegate_task` per hypothesis:
   * `isolation: "worktree"`;
   * target `opencode` / `opencode-go/hy4-flash-free` (variant `high`);
   * the task must be self-contained: goal, exact branch name, the hard
     rules from `docs/exp/README.md`, and a "do not run benchmarks
     locally" instruction;
   * require one focused commit, a push to `exp/<name>`, and a report.
5. Do not wait for the children in the same turn.  They will wake the
   thread when they finish.
6. When results arrive, download `exp-results.json`, compare, and:
   * if the branch is a clear win, open a PR to main with the evidence
     (do **not** merge automatically);
   * if it is a clear loss, record it in the backlog as rejected with the
     numbers;
   * if it is inconclusive, propose a higher-repetition rerun or a
     follow-up hypothesis.

## Adoption gate

A branch is PR-worthy only when all hold:

* the exp-bench job succeeded (correctness gates passed; `abicheck` is
  currently advisory because the runner toolchain fails `cxxcheck` on
  clean main);
* the diff touches only `src/fm_core.h` / `src/fm_static.c` (plus an ADR
  when adopted);
* `micro_geomean <= 1.00` and `micro_worst <= 1.01`;
* `real_paired_geomean <= 0.99` and `real_paired_worst <= 1.02`;
* **peak RSS is not worse than main on any app that moves**, and the
  branch moves at least one app closer to `v2 RSS <= best competitor`;
* a full matrix (new suite and, when the change affects it, the legacy
  dashboard suite) shows v2 first in both time and RSS on the targeted
  apps.

A speed-only branch that increases RSS is rejected, even if the time
geomean is excellent.  A footprint-only branch that gives back more than
1% time on any app is rejected unless it closes a much larger RSS gap.

Never auto-merge to main.  Open a PR with the numbers and let the owner
decide.  Never push benchmark-workflow or fixture changes from an
experiment branch.

## Budget and hygiene

* At most 6 new experiment branches per cycle, at most 20 concurrent
  GitHub Actions jobs overall.
* One hypothesis per branch; no bundling.
* Delete/close stale experiment branches only when the owner asks.
* If the free model starts producing non-compiling or no-op branches,
  reduce the fleet and switch to smaller, more explicit hypotheses.
