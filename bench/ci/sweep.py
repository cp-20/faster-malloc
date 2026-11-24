#!/usr/bin/env python3
"""sweep.py -- evaluate runtime FM_* policies against the same library.

Each config runs the default library as the baseline and the same library
with KEY=VALUE overrides as the candidate, using bench/ci/expbench.py, so
the comparison is paired and needs no rebuild.  This is the high-throughput
half of the campaign: every scheduled run tests a batch of knobs and ranks
them by both time and RSS.

usage: python3 bench/ci/sweep.py --lib ./libfastmalloc2.so \
           --reps 3 --out sweep.json
"""

from __future__ import annotations

import argparse
import json
import math
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXPBENCH = ROOT / "bench" / "ci" / "expbench.py"

CONFIGS = [
    ("default", {}),
    ("cold-purge", {"FM_LCACHE_COLD_PURGE": "1"}),
    ("profile-footprint", {"FM_PROFILE": "footprint"}),
    ("profile-throughput", {"FM_PROFILE": "throughput"}),
    ("profile-shortlived", {"FM_PROFILE": "shortlived"}),
    ("no-page-purge", {"FM_PAGE_PURGE": "0"}),
    ("purge-2", {"FM_PURGE_MIN_PAGES": "2"}),
    ("purge-6", {"FM_PURGE_MIN_PAGES": "6"}),
    ("purge-8", {"FM_PURGE_MIN_PAGES": "8"}),
    ("age-25", {"FM_RELEASE_AGE_MS": "25"}),
    ("age-50", {"FM_RELEASE_AGE_MS": "50"}),
    ("keep-0", {"FM_RELEASE_KEEP": "0"}),
    ("keep-1", {"FM_RELEASE_KEEP": "1"}),
    ("depth-2", {"FM_RELEASE_MIN_DEPTH": "2"}),
    ("depth-8", {"FM_RELEASE_MIN_DEPTH": "8"}),
    ("lcache-16m", {"FM_LCACHE_MAX_BYTES": "16777216"}),
    ("lcache-32m", {"FM_LCACHE_MAX_BYTES": "33554432"}),
    ("lcache-128m", {"FM_LCACHE_MAX_BYTES": "134217728"}),
    ("lcache-256m", {"FM_LCACHE_MAX_BYTES": "268435456"}),
    ("local-div-2", {"FM_LOCAL_LIMIT_DIV": "2"}),
    ("realloc-search", {"FM_REALLOC_CACHE_SEARCH": "1"}),
    ("lazy-bump-off", {"FM_LAZY_BUMP": "0"}),
    ("locality-sort", {"FM_LOCALITY_SORT": "1"}),
]

DEFAULT_APPS = "jq_large,python_data,sort_p4,gxx_compile,sqlite_oltp"


def geomean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else 1.0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--lib", required=True)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--apps", default=DEFAULT_APPS)
    ap.add_argument("--only", default="", help="comma-separated config names")
    ap.add_argument("--out", default="sweep.json")
    args = ap.parse_args()

    lib = str(Path(args.lib).resolve())
    if not Path(lib).exists():
        print(f"library missing: {lib}", file=sys.stderr)
        return 2
    only = {x for x in args.only.split(",") if x}
    configs = [c for c in CONFIGS if not only or c[0] in only]
    rows = []
    for name, env in configs:
        if name == "default":
            continue
        cmd = [
            sys.executable,
            str(EXPBENCH),
            "--baseline", lib,
            "--candidate", lib,
            "--skip-micro",
            "--reps", str(args.reps),
            "--apps", args.apps,
            "--json", f"/tmp/sweep-{name}.json",
        ]
        for k, v in env.items():
            cmd += ["--candidate-env", f"{k}={v}"]
        print(f"\n=== {name}: {' '.join(f'{k}={v}' for k, v in env.items())} ===", flush=True)
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if proc.returncode != 0:
            print(proc.stdout[-2000:])
            rows.append({"name": name, "env": env, "error": f"expbench rc={proc.returncode}"})
            continue
        d = json.loads(Path(f"/tmp/sweep-{name}.json").read_text())
        time_geo = d["score"].get("real_paired_geomean", d["score"].get("real_geomean", 1.0))
        rss_ratios = [v["candidate_rss_kb"] / v["baseline_rss_kb"] for v in d["real"].values()]
        rss_geo = geomean(rss_ratios)
        rows.append({
            "name": name,
            "env": env,
            "time_geomean": time_geo,
            "time_worst": d["score"].get("real_paired_worst", 9.0),
            "rss_geomean": rss_geo,
            "rss_worst": max(rss_ratios) if rss_ratios else 9.0,
            "real": {
                k: {
                    "time": v.get("paired_ratio", v["ratio"]),
                    "rss": v["candidate_rss_kb"] / v["baseline_rss_kb"],
                }
                for k, v in d["real"].items()
            },
        })
        print(f"{name:<20} time {time_geo:.4f} rss {rss_geo:.4f}", flush=True)

    rows.sort(key=lambda r: (r.get("time_geomean", 9) + r.get("rss_geomean", 9), r.get("rss_worst", 9)))
    out = {"reps": args.reps, "apps": args.apps, "configs": rows}
    Path(args.out).write_text(json.dumps(out, indent=2) + "\n")
    print(f"\n{'config':<20} {'time':>8} {'time_worst':>11} {'rss':>8} {'rss_worst':>10}")
    for r in rows:
        if "error" in r:
            print(f"{r['name']:<20} {r['error']}")
            continue
        print(f"{r['name']:<20} {r['time_geomean']:>8.4f} {r['time_worst']:>11.4f} "
              f"{r['rss_geomean']:>8.4f} {r['rss_worst']:>10.4f}")
    print(f"\nwrote {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
