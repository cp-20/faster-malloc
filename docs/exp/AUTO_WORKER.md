# Autonomous worker brief

You are one worker in a continuous faster-malloc optimization campaign.
You have a clean git worktree of the repository. Do one focused
experiment, then stop.

## Goal

Make `libfastmalloc2.so` first in **both wall time and peak RSS** on
every app in `docs/exp/GOAL.md`. Do not stop at a speed win that uses
more memory, and do not trade more than 1% time for a small RSS win.

## Pick one task

1. Read `docs/exp/GOAL.md`, `docs/exp/backlog.md`, and the relevant ADRs.
2. List the attempt log: `cat /tmp/fm-campaign/attempted 2>/dev/null`.
3. List live branches: `git ls-remote origin 'refs/heads/exp/*'`.
4. Pick the **first** open hypothesis in `docs/exp/backlog.md` that:
   - has no `exp/auto-<id>` branch and no obvious existing experiment
     branch for the same mechanism,
   - is not in the attempt log,
   - has a plausible, small implementation.
5. Append your chosen id to `/tmp/fm-campaign/attempted` immediately, so
   the next worker does not pick it again.

If nothing is left, print `NO TASK` and exit 0.

## Worktree

Create your own worktree, never edit the main checkout:

```sh
git fetch origin main
git worktree add -b exp/auto-<id> /tmp/fm-campaign/<id> origin/main
cd /tmp/fm-campaign/<id>
```

If the backlog item names a base experiment branch, use that branch as
the start instead of `origin/main`. Do not reuse an existing branch name.

## Hard rules

* Only edit `src/fm_core.h` (or `src/fm_static.c` if the change is
  entry-point-only). No `.github/`, `bench/`, `docs/`, `Makefile`, or
  generated headers.
* Never run benchmarks locally. No callgrind, cachegrind, suite.sh,
  real_suite.py, ab.py, or timing loops. Compile and run correctness:
  ```sh
  make -j"$(nproc)" libfastmalloc2.so libjitmalloc.so
  LD_PRELOAD=$PWD/libfastmalloc2.so ./test_basic
  LD_PRELOAD=$PWD/libfastmalloc2.so ./bench/stress
  ```
  and the same with `libjitmalloc.so`.
* One focused commit. Preserve the ABI and all allocator semantics.
* Push only your experiment branch:
  `git push origin HEAD:refs/heads/exp/auto-<id>`
* Never push to main. Never merge. Do not spawn subagents.
* If the idea is unsafe or clearly a no-op, do not push; report why and
  let the next worker take the next item.

## Report

Report: chosen id, branch, commit SHA, files/lines, expected time/RSS
effect, local tests, and risks. The push triggers `.github/workflows/exp-bench.yml`,
which measures time and RSS against main.
