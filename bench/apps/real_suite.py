#!/usr/bin/env python3
"""real_suite.py -- Large-scale real-application benchmark suite.

Measures wall time (seconds) and Peak RSS (KiB) via run_timed.py across
glibc, faster-malloc v2, mimalloc, jemalloc, and tcmalloc using round-robin
interleaving and output checksum verification.
"""
import hashlib
import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUN_TIMED = ROOT / "bench" / "apps" / "run_timed.py"
WORK = ROOT / "bench" / "apps" / "work" / "real_suite"


def find_allocators():
    matrix = os.environ.get("MATRIX", "0") == "1"
    allocs = [("glibc", "", {})]
    v2 = ROOT / "libfastmalloc2.so"
    if v2.exists():
        allocs.append(("v2_default", str(v2), {}))
        if matrix:
            allocs.append(("v2_footprint", str(v2), {"FM_PROFILE": "footprint"}))
            allocs.append(("v2_shortlived", str(v2), {"FM_PROFILE": "shortlived"}))
            allocs.append(("v2_throughput", str(v2), {"FM_PROFILE": "throughput"}))
            allocs.append(("v2_locality", str(v2), {"FM_LOCALITY_SORT": "1"}))

    candidates = {
        "mimalloc": ["libmimalloc.so.2", "libmimalloc.so"],
        "jemalloc": ["libjemalloc.so.2", "libjemalloc.so"],
        "tcmalloc": ["libtcmalloc_minimal.so.4", "libtcmalloc.so.4"],
    }
    try:
        ld = subprocess.check_output(["ldconfig", "-p"], text=True)
    except Exception:
        ld = ""

    for name, sonames in candidates.items():
        path = None
        for so in sonames:
            for line in ld.splitlines():
                if so in line and "=>" in line:
                    p = line.split("=>", 1)[1].strip()
                    if os.path.exists(p):
                        path = p
                        break
            if path:
                break
        if path:
            allocs.append((name, path, {}))
    return allocs


def prepare_fixtures():
    WORK.mkdir(parents=True, exist_ok=True)

    # 1. Large C++20/STL compilation fixture
    cpp_file = WORK / "heavy.cpp"
    if not cpp_file.exists():
        lines = [
            "#include <algorithm>",
            "#include <cstdint>",
            "#include <numeric>",
            "#include <string>",
            "#include <unordered_map>",
            "#include <vector>",
            "",
            "template <int N>",
            "struct Node {",
            "  std::vector<uint64_t> data;",
            "  std::unordered_map<std::string, int> idx;",
            "  uint64_t run(int k) {",
            "    data.resize(64 + (N % 37));",
            "    std::iota(data.begin(), data.end(), uint64_t(k + N));",
            "    std::sort(data.rbegin(), data.rend());",
            "    idx[std::to_string(N)] = k;",
            "    return data[0] + idx.size();",
            "  }",
            "};",
        ]
        for i in range(180):
            lines.append(f"uint64_t fn_{i}(int k) {{ Node<{i}> n; return n.run(k); }}")
        lines.append("uint64_t entry(int k) { uint64_t s = 0;")
        for i in range(180):
            lines.append(f"  s += fn_{i}(k + {i});")
        lines.append("  return s; }")
        cpp_file.write_text("\n".join(lines) + "\n")

    # 2. Large JSON fixture for jq (~35 MB, 250k records)
    json_file = WORK / "big_records.json"
    if not json_file.exists():
        with json_file.open("w") as f:
            f.write("[\n")
            for i in range(250000):
                active = "true" if (i % 3 != 0) else "false"
                val = (i * 2654435761) & 0xFFFF
                comma = "," if i < 249999 else ""
                f.write(
                    f'{{"id":{i},"group":"g{i%64}","active":{active},'
                    f'"val":{val},"tag":"item_{i%997}"}}{comma}\n'
                )
            f.write("]\n")

    # 3. SQLite 200k-row DB + query script
    db_file = WORK / "oltp.db"
    sql_script = WORK / "queries.sql"
    if not db_file.exists():
        import sqlite3
        conn = sqlite3.connect(str(db_file))
        cur = conn.cursor()
        cur.execute("PRAGMA journal_mode=WAL;")
        cur.execute("CREATE TABLE events(id INTEGER PRIMARY KEY, grp INT, score INT, payload TEXT);")
        rows = [(i, i % 128, (i * 48271) % 100000, f"payload_{i%500}_{'x'*24}") for i in range(200000)]
        cur.executemany("INSERT INTO events VALUES (?,?,?,?);", rows)
        cur.execute("CREATE INDEX idx_grp_score ON events(grp, score);")
        conn.commit()
        conn.close()
    if not sql_script.exists():
        sql_script.write_text(
            "PRAGMA temp_store=MEMORY;\n"
            "SELECT grp, COUNT(*), SUM(score), MAX(payload) FROM events GROUP BY grp ORDER BY SUM(score) DESC LIMIT 20;\n"
            "SELECT e1.grp, COUNT(*) FROM events e1 JOIN events e2 ON e1.id = e2.id + 1 WHERE e1.score > 50000 GROUP BY e1.grp LIMIT 20;\n"
            "SELECT * FROM events WHERE score BETWEEN 10000 AND 12000 ORDER BY payload, id LIMIT 100;\n"
        )

    # 4. Python data pipeline script
    py_script = WORK / "py_pipeline.py"
    if not py_script.exists():
        py_script.write_text(
            "import hashlib, json\n"
            "recs = [{'id': i, 'k': f'k_{i%200}', 'v': (i * 1315423911) & 0xffff} for i in range(180000)]\n"
            "s = json.dumps(recs)\n"
            "parsed = json.loads(s)\n"
            "buckets = {}\n"
            "for r in parsed:\n"
            "    buckets.setdefault(r['k'], []).append(r['v'])\n"
            "tot = sum(sum(sorted(v)) for v in buckets.values())\n"
            "bufs = [bytearray(128 * 1024 + (i % 7) * 8192) for i in range(64)]\n"
            "for b in bufs: b[0] = 1; b[-1] = 2\n"
            "print(hashlib.sha256(f'{tot}:{len(s)}'.encode()).hexdigest())\n"
        )

    # 5. Git 1500-commit repo
    git_dir = WORK / "repo"
    if not git_dir.exists():
        git_dir.mkdir(parents=True)
        subprocess.run(["git", "init", "-q"], cwd=git_dir, check=True)
        subprocess.run(["git", "config", "user.email", "bench@example.com"], cwd=git_dir, check=True)
        subprocess.run(["git", "config", "user.name", "Bench"], cwd=git_dir, check=True)
        fast_import = []
        for i in range(1, 1501):
            msg = f"commit {i}\n"
            data = f"file content {i} " * 20 + "\n"
            fast_import.append(
                f"commit refs/heads/main\n"
                f"mark :{i}\n"
                f"author Bench <bench@example.com> {1700000000 + i} +0000\n"
                f"committer Bench <bench@example.com> {1700000000 + i} +0000\n"
                f"data {len(msg)}\n{msg}"
                + (f"from :{i-1}\n" if i > 1 else "")
                + f"M 100644 inline f{i%32}.txt\n"
                f"data {len(data)}\n{data}\n"
            )
        subprocess.run(
            ["git", "fast-import", "--quiet"],
            input="".join(fast_import),
            text=True,
            cwd=git_dir,
            check=True,
        )
        subprocess.run(["git", "checkout", "-q", "main"], cwd=git_dir, check=True)

    # 6. Structured text payload (12 MB) for xz & sort
    xz_input = WORK / "payload.txt"
    if not xz_input.exists():
        with xz_input.open("w") as f:
            for i in range(250000):
                key = (i * 2654435761) % 1000000
                f.write(f"{key:07d}\tevent_{i%128}\tuser_{i%4096}\tmsg_payload_data_{i%997}\n")


def build_env(lib_path, extra_env):
    env = os.environ.copy()
    for k in ("FM_PROFILE", "FM_LOCAL_LIMIT_DIV", "FM_LCACHE_COLD_PURGE", "FM_LAZY_BUMP", "FM_LOCALITY_SORT"):
        env.pop(k, None)
    if lib_path:
        env["LD_PRELOAD"] = lib_path
    else:
        env.pop("LD_PRELOAD", None)
    for k, v in (extra_env or {}).items():
        env[k] = v
    return env


def run_one(cmd, lib_path, extra_env=None, cwd=None):
    env = build_env(lib_path, extra_env)
    # Run via run_timed.py; capture stdout for checksum and stderr for R3TIME
    proc = subprocess.run(
        ["python3", str(RUN_TIMED)] + cmd,
        env=env,
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    stderr_txt = proc.stderr.decode("utf-8", errors="replace")
    elapsed = None
    rss_kb = None
    for line in stderr_txt.splitlines():
        if line.startswith("R3TIME "):
            parts = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
            elapsed = float(parts["elapsed"])
            rss_kb = int(parts["maxrss_kb"])
    if proc.returncode != 0 or elapsed is None:
        raise RuntimeError(f"Command failed ({proc.returncode}): {cmd}\n{stderr_txt}")
    digest = hashlib.sha256(proc.stdout).hexdigest()[:16]
    return elapsed, rss_kb, digest


def main():
    reps = int(os.environ.get("REPS", "3"))
    only = os.environ.get("APPS", "").split()
    prepare_fixtures()
    allocators = find_allocators()
    print(f"Allocators: {[a[0] for a in allocators]}, REPS={reps}")

    cases = [
        ("g++_compile", ["g++", "-O2", "-c", str(WORK / "heavy.cpp"), "-o", str(WORK / "heavy.o")], None),
        ("jq_large", ["jq", "-c", "[.[] | select(.active) | {id, s: (.val * 2)}] | sort_by(.s) | length", str(WORK / "big_records.json")], None),
        ("sqlite_oltp", ["sqlite3", "-readonly", str(WORK / "oltp.db")], None),
        ("python_data", ["python3", str(WORK / "py_pipeline.py")], None),
        ("git_revlist", ["sh", "-c", "git log --oneline >/dev/null && git rev-list --objects --all | wc -l"], WORK / "repo"),
        ("xz_mt2", ["xz", "-T2", "-4", "-c", str(WORK / "payload.txt")], None),
        ("sort_p4", ["sort", "--parallel=4", "-S", "128M", "-k1,1n", str(WORK / "payload.txt")], None),
    ]
    if only:
        cases = [c for c in cases if c[0] in only]

    # Special handling for sqlite_oltp stdin
    sql_bytes = (WORK / "queries.sql").read_bytes()

    results = {}
    for cname, cmd, cwd in cases:
        results[cname] = {name: {"times": [], "rss": []} for name, _, _ in allocators}
        ref_digest = None
        for r in range(reps):
            order = allocators[r % len(allocators):] + allocators[:r % len(allocators)]
            for name, path, extra_env in order:
                if cname == "sqlite_oltp":
                    env = build_env(path, extra_env)
                    proc = subprocess.run(
                        ["python3", str(RUN_TIMED)] + cmd,
                        input=sql_bytes,
                        env=env,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                    )
                    stderr_txt = proc.stderr.decode("utf-8", errors="replace")
                    elapsed, rss_kb = None, None
                    for line in stderr_txt.splitlines():
                        if line.startswith("R3TIME "):
                            parts = dict(kv.split("=", 1) for kv in line.split()[1:] if "=" in kv)
                            elapsed = float(parts["elapsed"])
                            rss_kb = int(parts["maxrss_kb"])
                    if proc.returncode != 0 or elapsed is None:
                        raise RuntimeError(f"sqlite failed: {stderr_txt}")
                    digest = hashlib.sha256(proc.stdout).hexdigest()[:16]
                else:
                    elapsed, rss_kb, digest = run_one(cmd, path, extra_env=extra_env, cwd=cwd)
                    if cname == "g++_compile":
                        digest = hashlib.sha256((WORK / "heavy.o").read_bytes()).hexdigest()[:16]
                if ref_digest is None:
                    ref_digest = digest
                elif digest != ref_digest:
                    raise AssertionError(f"Checksum mismatch for {cname}/{name}: {digest} != {ref_digest}")
                results[cname][name]["times"].append(elapsed)
                results[cname][name]["rss"].append(rss_kb)
        print(f"done: {cname}")

    print("\n=== REAL-APP SUITE SUMMARY (median [min..max]) ===")
    header = f"{'workload':<14} " + " ".join(f"{name:>22}" for name, _, _ in allocators)
    print(header)
    for cname, _, _ in cases:
        row_t = [f"{cname + ' (s)':<14}"]
        row_r = [f"{'  RSS (MiB)':<14}"]
        for name, _, _ in allocators:
            ts = results[cname][name]["times"]
            rs = results[cname][name]["rss"]
            row_t.append(f"{statistics.median(ts):6.3f} [{min(ts):.2f}-{max(ts):.2f}]".rjust(22))
            row_r.append(f"{statistics.median(rs)/1024:6.1f} [{min(rs)/1024:.0f}-{max(rs)/1024:.0f}]".rjust(22))
        print(" ".join(row_t))
        print(" ".join(row_r))

    out_json = WORK / "results.json"
    out_json.write_text(json.dumps(results, indent=2))
    print(f"\nSaved raw results to {out_json}")


if __name__ == "__main__":
    main()
