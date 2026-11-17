#!/usr/bin/env python3
"""build_dashboard.py -- Append benchmark results to history.json & data.js."""
import argparse
import json
import statistics
from datetime import datetime, timezone
from pathlib import Path


def summarize_real_suite(raw: dict) -> dict:
    out = {}
    for workload, allocs in raw.items():
        out[workload] = {}
        for alloc, metrics in allocs.items():
            ts = metrics.get("times", [])
            rs = metrics.get("rss", [])
            if not ts or not rs:
                continue
            out[workload][alloc] = {
                "time_med": round(statistics.median(ts), 4),
                "time_min": round(min(ts), 4),
                "time_max": round(max(ts), 4),
                "rss_med_kb": int(statistics.median(rs)),
                "rss_min_kb": int(min(rs)),
                "rss_max_kb": int(max(rs)),
            }
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--real-suite", default="")
    ap.add_argument("--cachegrind", default="")
    ap.add_argument("--commit", default="local")
    ap.add_argument("--message", default="")
    ap.add_argument("--timestamp", default="")
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    hist_path = out_dir / "history.json"

    history = []
    if hist_path.exists():
        try:
            history = json.loads(hist_path.read_text())
        except Exception:
            history = []

    entry = {
        "commit": args.commit[:12],
        "message": args.message.splitlines()[0][:80] if args.message else "",
        "timestamp": args.timestamp or datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "real_suite": {},
        "cachegrind": {},
    }

    if args.real_suite and Path(args.real_suite).exists():
        raw = json.loads(Path(args.real_suite).read_text())
        entry["real_suite"] = summarize_real_suite(raw)

    if args.cachegrind and Path(args.cachegrind).exists():
        entry["cachegrind"] = json.loads(Path(args.cachegrind).read_text())

    # Replace if same commit (except 'local'), else append
    if entry["commit"] != "local":
        history = [h for h in history if h.get("commit") != entry["commit"]]
    history.append(entry)
    history = history[-100:]

    hist_json = json.dumps(history, indent=2)
    hist_path.write_text(hist_json + "\n")
    (out_dir / "data.js").write_text(f"window.BENCH_HISTORY = {hist_json};\n")
    print(f"Updated dashboard data ({len(history)} entries) in {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
