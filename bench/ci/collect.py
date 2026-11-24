#!/usr/bin/env python3
"""collect.py -- rank exp/* branches by their latest exp-bench result.

For each branch, find the newest exp-bench run, download its artifact, and
print the micro/real geomean ratios.  Read-only: it never pushes or edits.

usage: python3 bench/ci/collect.py exp/startup exp/dense-thread ...
       python3 bench/ci/collect.py --all
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def gh_json(args: list[str]):
    out = subprocess.check_output(["gh"] + args, text=True)
    return json.loads(out)


def latest_run(branch: str):
    runs = gh_json(
        [
            "run",
            "list",
            "--branch",
            branch,
            "--workflow",
            "exp-bench",
            "--limit",
            "1",
            "--json",
            "databaseId,status,conclusion,url,headSha,createdAt",
        ]
    )
    return runs[0] if runs else None


def all_branches() -> list[str]:
    data = gh_json(
        [
            "api",
            "repos/{owner}/{repo}/branches?per_page=100",
            "--jq",
            "[.[].name]",
        ]
    )
    return [b for b in data if b.startswith("exp/")]


def fetch_result(branch: str, run: dict):
    run_id = run["databaseId"]
    with tempfile.TemporaryDirectory(prefix="expbench-") as td:
        subprocess.run(
            ["gh", "run", "download", str(run_id), "-n", f"exp-bench-{run_id}", "-D", td],
            check=True,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        path = Path(td) / "exp-results.json"
        if not path.exists():
            return None
        return json.loads(path.read_text())


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("branches", nargs="*")
    ap.add_argument("--all", action="store_true")
    args = ap.parse_args()
    branches = args.branches or (all_branches() if args.all else [])
    if not branches:
        print("no branches given (use --all)", file=sys.stderr)
        return 2

    rows = []
    for branch in branches:
        run = latest_run(branch)
        if not run:
            rows.append((branch, None, None))
            continue
        if run.get("conclusion") != "success":
            rows.append((branch, run, None))
            continue
        try:
            res = fetch_result(branch, run)
        except subprocess.CalledProcessError:
            res = None
        rows.append((branch, run, res))

    good = [r for r in rows if r[2] and r[2].get("score")]
    good.sort(key=lambda r: r[2]["score"].get("real_geomean", 9.0))

    print(f"{'branch':<26} {'real_geo':>9} {'micro_geo':>10} {'micro_worst':>12} {'run':>12}")
    for branch, run, res in good:
        s = res["score"]
        print(
            f"{branch:<26} {s.get('real_geomean', float('nan')):>9.4f} "
            f"{s.get('micro_geomean', float('nan')):>10.4f} "
            f"{s.get('micro_worst', float('nan')):>12.4f} "
            f"{run['databaseId']:>12}"
        )
        for app, d in res.get("real", {}).items():
            print(f"    {app:<16} {d['ratio']:.4f}  ({d['baseline_median_s']:.3f}s -> {d['candidate_median_s']:.3f}s)")
    for branch, run, res in rows:
        if res:
            continue
        if run is None:
            print(f"{branch:<26} no exp-bench run")
        else:
            print(f"{branch:<26} {run.get('conclusion')} {run.get('url')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
