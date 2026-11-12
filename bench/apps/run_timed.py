#!/usr/bin/env python3
"""run_timed.py -- run one command, report wall time and peak RSS.

Prints the command's own stdout/stderr through, then a final line:
    R3TIME elapsed=<seconds> maxrss_kb=<n> exit=<code>
maxrss comes from wait4 rusage: for a command tree it is the largest
peak RSS among the waited-for processes (for a compiler driver that
means cc1, which is the process we care about).  Used instead of GNU
time so the harness has no extra dependency.
"""
import os
import resource
import subprocess
import sys
import time


def main() -> int:
    if len(sys.argv) < 2:
        print("usage: run_timed.py cmd...", file=sys.stderr)
        return 2
    start = time.monotonic()
    proc = subprocess.run(sys.argv[1:])
    elapsed = time.monotonic() - start
    ru = resource.getrusage(resource.RUSAGE_CHILDREN)
    print(
        f"R3TIME elapsed={elapsed:.3f} maxrss_kb={ru.ru_maxrss} exit={proc.returncode}",
        file=sys.stderr,
    )
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
