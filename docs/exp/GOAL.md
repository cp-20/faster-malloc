# Optimization goal

Make `libfastmalloc2.so` (v2) the fastest allocator on the repository's
benchmark suite, without regressing correctness, ABI, or peak RSS.

This file is the standing brief for the recurring optimization campaign.
Every campaign cycle starts by reading it and `docs/exp/backlog.md`.

## Scoreboard

Two signals, in this order:

1. **Exact micro instruction counts** from `bench/ci/expbench.py`:
   `score.micro_geomean`, `score.micro_worst`.  Reproducible bit-for-bit.
2. **Real applications** from the same job:
   `score.real_paired_geomean`, `score.real_paired_worst`, plus per-app
   `paired_ratio` and peak RSS.  `real_geomean` (ratio of medians) is a
   secondary view; short apps such as `sort` are noisy, so prefer the
   paired statistic and the raw times.

The public claim is the full matrix from `apps-bench` (v2 vs glibc,
mimalloc, jemalloc, tcmalloc), not the ratio against main alone.

## Baseline

The campaign starts from whatever is on `main`.  Accepted mechanisms so
far (see ADRs 0079-0082):

* full 64 KiB per-class local byte budget;
* one-request headroom for geometric large realloc growth;
* adaptive warm large-cache budget (2x, capped at 128 MiB);
* 4 MiB per-thread large slab.

`main` is the merge base; every experiment is screened against it in the
same CI job.

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
* `real_paired_geomean <= 0.99` and `real_paired_worst <= 1.02`, with
  RSS flat or better on the apps that move;
* a full `apps-bench` matrix on the branch shows v2 at or near the front.

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
