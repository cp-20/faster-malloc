#!/usr/bin/env python3
"""ab.py -- interleaved A/B of a candidate libfastmalloc2.so vs a baseline and
the system allocators on real programs. Median wall time / peak RSS.

usage: REPS=7 python3 bench/apps/ab.py <candidate.so> [baseline.so]
Fixtures: bench/apps/work/real_suite (built by real_suite.py).
"""
import os, statistics as st, subprocess, sys, time
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
W = str(Path(os.environ.get("FIXTURES", ROOT / "bench/apps/work/real_suite")))
L = "/lib/x86_64-linux-gnu/"
A = {"glibc": "", "v2_base": sys.argv[2] if len(sys.argv) > 2 else str(ROOT / "libfastmalloc2.so"),
     "v2_new": sys.argv[1], "mimalloc": L + "libmimalloc.so.2", "jemalloc": L + "libjemalloc.so.2",
     "tcmalloc": L + "libtcmalloc_minimal.so.4"}
A = {k: v for k, v in A.items() if not v or os.path.exists(v)}
WK = {"jq": ["jq", "-c", "[.[] | select(.active) | {id, s: (.val * 2)}] | sort_by(.s) | length", W + "/big_records.json"],
      "python": ["python3", W + "/py_pipeline.py"],
      "sort": ["sort", "--parallel=4", "-S", "128M", "-k1,1n", W + "/payload.txt"],
      "xz": ["sh", "-c", "xz -T2 -6 -c " + W + "/payload.txt >/dev/null"],
      "gxx": ["g++", "-std=c++20", "-O2", "-c", W + "/heavy.cpp", "-o", "/dev/null"],
      "git": ["git", "-C", W + "/repo", "rev-list", "--objects", "--all"]}
only = os.environ.get("ONLY")
if only:
    WK = {k: v for k, v in WK.items() if k in only.split(",")}
R = int(os.environ.get("REPS", "7"))
res = {}
for w, c in WK.items():
    for _ in range(R):
        for a, l in A.items():
            e = dict(os.environ)
            if l:
                e["LD_PRELOAD"] = l
            t = time.perf_counter()
            p = subprocess.run(["/usr/bin/time", "-f", "%M"] + c, env=e, stdout=subprocess.DEVNULL,
                               stderr=subprocess.PIPE, text=True)
            t = time.perf_counter() - t
            if p.returncode != 0:
                sys.exit(f"FAIL {w} {a}: {p.stderr[-300:]}")
            res.setdefault((w, a), []).append((t, int(p.stderr.strip().split()[-1])))
    print(w, "  ".join(f"{a}={st.median(x[0] for x in res[(w, a)]):.3f}s/{st.median(x[1] for x in res[(w, a)]) // 1024}M"
                       for a in A), flush=True)
