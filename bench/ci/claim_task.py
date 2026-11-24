#!/usr/bin/env python3
"""claim_task.py -- atomically claim one backlog task for a parallel worker.

Prints: id<TAB>branch<TAB>area<TAB>hypothesis
Exit 1 when no task is left.

The claim is recorded in /tmp/fm-campaign/claimed (global across workers).
State is intentionally outside the repository so a claim never creates a
main-branch commit.
"""

from __future__ import annotations

import fcntl
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
STATE = Path("/tmp/fm-campaign")
STATE.mkdir(parents=True, exist_ok=True)
BACKLOG = ROOT / "docs" / "exp" / "backlog.md"


def read_ids(path: Path) -> set[str]:
    if not path.exists():
        return set()
    return {x for x in path.read_text().split() if x}


def main() -> int:
    text = BACKLOG.read_text()
    sections = ["## Open hypotheses", "## Autonomous queue (new worker tasks)"]
    items: list[tuple[str, str, str, str]] = []
    for name in sections:
        if name not in text:
            continue
        sec = text.split(name, 1)[1].split("\n## ", 1)[0]
        for line in sec.splitlines():
            m = re.match(r"\|\s*([A-Z]\d+)\s*\|\s*([^|]+)\|\s*([^|]+)\|\s*([^|]+)\|", line)
            if m:
                items.append(tuple(x.strip() for x in m.groups()))
    if not items:
        print("no backlog items", file=sys.stderr)
        return 1

    lock_path = STATE / "claim.lock"
    with lock_path.open("w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        claimed = read_ids(STATE / "claimed")
        attempted = read_ids(STATE / "attempted")
        try:
            remotes = subprocess.check_output(
                ["git", "ls-remote", "origin", "refs/heads/exp/*"],
                cwd=ROOT, text=True, stderr=subprocess.DEVNULL,
            )
        except subprocess.CalledProcessError:
            remotes = ""
        existing = {
            line.split("refs/heads/", 1)[1].strip()
            for line in remotes.splitlines() if "refs/heads/" in line
        }
        # Prefer the N queue, then the older open hypotheses.
        items.sort(key=lambda x: (0 if x[0].startswith("N") else 1, x[0]))
        for tid, area, hyp, risk in items:
            branch = f"exp/auto-{tid.lower()}"
            if tid in claimed or tid in attempted or branch in existing:
                continue
            with (STATE / "claimed").open("a") as fh:
                fh.write(tid + "\n")
            print("\t".join([tid, branch, area, hyp]))
            return 0
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
