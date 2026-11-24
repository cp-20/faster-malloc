#!/usr/bin/env python3
"""expbench.py -- screen one allocator candidate against a baseline on CI.

Two layers of evidence, mirroring bench/suite.sh but reduced to what can
run inside one GitHub Actions job:

  * callgrind instruction counts on the bench/instr micro workloads
    (exact, noise-free; the primary signal);
  * interleaved wall-clock time and peak RSS on the real-application
    fixtures from bench/apps/real_suite.py (the signal that decides the
    "fastest on real programs" claim; noisier, so it is interleaved and
    reduced to a median).

The baseline and the candidate are run in the same process of the same
job, alternating order every repetition, so runner drift hits both.

Usage:
    python3 bench/ci/expbench.py \
        --baseline /tmp/fm-base/libfastmalloc2.so \
        --candidate ./libfastmalloc2.so \
        --reps 3 --apps jq_large,python_data,sort_p4,gxx_compile,git_revlist,sqlite_oltp \
        --json exp-results.json

Exit status is 0 even when the candidate loses: the caller (the workflow,
or a human) decides.  Set --fail-on-regression to turn a bad score into a
non-zero exit.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "bench" / "apps"))

import real_suite  # noqa: E402

MICRO = [
    ("churn", 100000, 64),
    ("churn", 100000, 8),
    ("churn", 100000, 1024),
    ("churnmix", 100000, None),
    ("mixed", 80000, None),
    ("retain", 80000, None),
    ("cross", 80000, None),
]

ALL_APPS = [
    "jq_large",
    "python_data",
    "sort_p4",
    "gxx_compile",
    "git_revlist",
    "sqlite_oltp",
    "xz_mt2",
]
DEFAULT_APPS = "jq_large,python_data,sort_p4,gxx_compile,git_revlist,sqlite_oltp"

# Commands copied from bench/apps/real_suite.py so the two harnesses stay
# comparable.  The [(app)] entries are filled with fixture paths below.
APP_SPECS = {
    "gxx_compile": {
        "cmd": ["g++", "-O2", "-c", "{heavy}", "-o", "{heavy_obj}"],
        "digest_file": "{heavy_obj}",
    },
    "jq_large": {
        "cmd": [
            "jq",
            "-c",
            "[.[] | select(.active) | {id, s: (.val * 2)}] | sort_by(.s) | length",
            "{records}",
        ],
    },
    "sqlite_oltp": {
        "cmd": ["sqlite3", "-readonly", "{db}"],
        "stdin_file": "{sql}",
    },
    "python_data": {"cmd": ["python3", "{pipeline}"]},
    "git_revlist": {
        "cmd": ["sh", "-c", "git log --oneline >/dev/null && git rev-list --objects --all | wc -l"],
        "cwd": "{repo}",
    },
    "xz_mt2": {"cmd": ["xz", "-T2", "-4", "-c", "{payload}"]},
    "sort_p4": {
        "cmd": ["sort", "--parallel=4", "-S", "128M", "-k1,1n", "{payload}"],
    },
}


def fixture_paths(work: Path) -> dict[str, str]:
    return {
        "heavy": str(work / "heavy.cpp"),
        "heavy_obj": str(work / "heavy.o"),
        "records": str(work / "big_records.json"),
        "db": str(work / "oltp.db"),
        "sql": str(work / "queries.sql"),
        "pipeline": str(work / "py_pipeline.py"),
        "repo": str(work / "repo"),
        "payload": str(work / "payload.txt"),
    }


def expand(spec: dict, paths: dict[str, str]) -> dict:
    out = dict(spec)
    out["cmd"] = [part.format(**paths) for part in spec["cmd"]]
    if "cwd" in spec:
        out["cwd"] = spec["cwd"].format(**paths)
    if "stdin_file" in spec:
        out["stdin_file"] = spec["stdin_file"].format(**paths)
    if "digest_file" in spec:
        out["digest_file"] = spec["digest_file"].format(**paths)
    return out


def parse_r3time(stderr: bytes) -> tuple[float, int] | None:
    for line in stderr.decode("utf-8", errors="replace").splitlines():
        if line.startswith("R3TIME "):
            parts = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
            try:
                return float(parts["elapsed"]), int(parts["maxrss_kb"])
            except (KeyError, ValueError):
                return None
    return None


def run_real(spec: dict, lib: str) -> tuple[float, int, str]:
    env = real_suite.build_env(lib, None)
    stdin_bytes = None
    if spec.get("stdin_file"):
        stdin_bytes = Path(spec["stdin_file"]).read_bytes()
    proc = subprocess.run(
        ["python3", str(real_suite.RUN_TIMED)] + spec["cmd"],
        env=env,
        cwd=spec.get("cwd"),
        input=stdin_bytes,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"command failed ({proc.returncode}): {' '.join(spec['cmd'])}\n"
            + proc.stderr.decode("utf-8", errors="replace")[-2000:]
        )
    timed = parse_r3time(proc.stderr)
    if timed is None:
        raise RuntimeError(f"no R3TIME line: {' '.join(spec['cmd'])}")
    elapsed, rss_kb = timed
    if spec.get("digest_file"):
        digest = hashlib.sha256(Path(spec["digest_file"]).read_bytes()).hexdigest()[:16]
    else:
        digest = hashlib.sha256(proc.stdout).hexdigest()[:16]
    return elapsed, rss_kb, digest


def micro_instr(lib: str, mode: str, n: int, size: int | None) -> int:
    with tempfile.NamedTemporaryFile(prefix="cg-", suffix=".out", delete=False) as fh:
        out = Path(fh.name)
    try:
        cmd = [
            "valgrind",
            "--tool=callgrind",
            f"--callgrind-out-file={out}",
            "--quiet",
            str(ROOT / "bench" / "instr"),
            mode,
            str(n),
        ]
        if size is not None:
            cmd.append(str(size))
        env = os.environ.copy()
        env["LD_PRELOAD"] = lib
        subprocess.run(cmd, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        text = out.read_text(errors="replace")
        for line in text.splitlines():
            if line.startswith("summary:"):
                return int(line.split()[1])
        raise RuntimeError(f"no summary in callgrind output for {mode} {n} {size}")
    finally:
        out.unlink(missing_ok=True)


def geomean(values: list[float]) -> float:
    if not values:
        return 1.0
    return math.exp(sum(math.log(v) for v in values) / len(values))


def fmt_ratio(x: float) -> str:
    return f"{x:.4f}"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--candidate", required=True)
    ap.add_argument("--reps", type=int, default=3)
    ap.add_argument("--apps", default=DEFAULT_APPS)
    ap.add_argument("--skip-micro", action="store_true")
    ap.add_argument("--skip-real", action="store_true")
    ap.add_argument("--json", default="exp-results.json")
    ap.add_argument("--fail-on-regression", action="store_true")
    args = ap.parse_args()

    baseline = str(Path(args.baseline).resolve())
    candidate = str(Path(args.candidate).resolve())
    for label, lib in (("baseline", baseline), ("candidate", candidate)):
        if not Path(lib).exists():
            print(f"{label} library missing: {lib}", file=sys.stderr)
            return 2
    if not (ROOT / "bench" / "instr").exists():
        print("bench/instr missing; run make first", file=sys.stderr)
        return 2

    apps = [a.strip() for a in args.apps.split(",") if a.strip()]
    unknown = [a for a in apps if a not in APP_SPECS]
    if unknown:
        print(f"unknown apps: {', '.join(unknown)}", file=sys.stderr)
        return 2

    print(f"baseline:  {baseline}")
    print(f"candidate: {candidate}")
    print(f"reps={args.reps} apps={','.join(apps)}")

    result: dict = {
        "baseline": baseline,
        "candidate": candidate,
        "reps": args.reps,
        "micro": {},
        "real": {},
    }

    if not args.skip_micro:
        print("\n== callgrind instruction counts (lower is better) ==")
        print(f"{'workload':<24} {'baseline':>12} {'candidate':>12} {'cand/base':>10}")
        ratios = []
        for mode, n, size in MICRO:
            label = f"{mode} {n}" + (f" {size}" if size is not None else "")
            b = micro_instr(baseline, mode, n, size)
            c = micro_instr(candidate, mode, n, size)
            ratio = c / b
            ratios.append(ratio)
            result["micro"][label] = {"baseline": b, "candidate": c, "ratio": ratio}
            print(f"{label:<24} {b:>12} {c:>12} {ratio:>10.4f}")
        result["score"] = result.get("score", {})
        result["score"]["micro_geomean"] = geomean(ratios)
        result["score"]["micro_worst"] = max(ratios)
        print(f"{'geomean':<24} {'':>12} {'':>12} {result['score']['micro_geomean']:>10.4f}")

    if not args.skip_real and apps:
        print("\n== real applications, interleaved (lower is better) ==")
        real_suite.prepare_fixtures()
        paths = fixture_paths(real_suite.WORK)
        ratios = []
        for app in apps:
            spec = expand(APP_SPECS[app], paths)
            data = {"baseline": {"times": [], "rss": []}, "candidate": {"times": [], "rss": []}}
            ref_digest = None
            for rep in range(args.reps):
                order = ("baseline", "candidate") if rep % 2 == 0 else ("candidate", "baseline")
                for who in order:
                    lib = baseline if who == "baseline" else candidate
                    elapsed, rss, digest = run_real(spec, lib)
                    if ref_digest is None:
                        ref_digest = digest
                    elif digest != ref_digest:
                        raise AssertionError(f"checksum mismatch for {app}/{who}")
                    data[who]["times"].append(elapsed)
                    data[who]["rss"].append(rss)
            b = statistics.median(data["baseline"]["times"])
            c = statistics.median(data["candidate"]["times"])
            ratio = c / b if b else float("inf")
            ratios.append(ratio)
            result["real"][app] = {
                "baseline_median_s": b,
                "candidate_median_s": c,
                "baseline_min_s": min(data["baseline"]["times"]),
                "candidate_min_s": min(data["candidate"]["times"]),
                "baseline_rss_kb": int(statistics.median(data["baseline"]["rss"])),
                "candidate_rss_kb": int(statistics.median(data["candidate"]["rss"])),
                "ratio": ratio,
            }
            print(
                f"{app:<16} base {b:.4f}s cand {c:.4f}s ratio {ratio:.4f} "
                f"| RSS {result['real'][app]['baseline_rss_kb']//1024}M -> "
                f"{result['real'][app]['candidate_rss_kb']//1024}M"
            )
        result["score"] = result.get("score", {})
        result["score"]["real_geomean"] = geomean(ratios)
        result["score"]["real_worst"] = max(ratios)
        print(f"{'geomean':<16} {'':>10} {'':>10} ratio {result['score']['real_geomean']:.4f}")

    Path(args.json).write_text(json.dumps(result, indent=2) + "\n")
    print(f"\nwrote {args.json}")

    if args.fail_on_regression:
        score = result.get("score", {})
        if score.get("micro_geomean", 1.0) >= 1.0 or score.get("real_geomean", 1.0) >= 1.0:
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
