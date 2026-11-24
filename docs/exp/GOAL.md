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
