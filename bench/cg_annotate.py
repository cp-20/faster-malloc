#!/usr/bin/env python3
"""cg_annotate.py -- extract summary counters from a cachegrind output file.

cachegrind's "events:" line lists the event names separated by whitespace
(not "name: value"), and the "summary:" line lists the values in the same
order.  Parse them positionally.
"""
import sys
import re


def parse(path):
    names = []
    with open(path) as f:
        for line in f:
            if line.startswith("events:"):
                names = line.split(":", 1)[1].split()
            elif line.startswith("summary:"):
                vals = [int(x) for x in re.findall(r"\d+", line)]
                return dict(zip(names, vals))
    return {}


if __name__ == "__main__":
    s = parse(sys.argv[1])
    ir = s.get("Ir", 0)
    d1 = s.get("D1mr", 0)
    bc = s.get("Bc", 0)
    bcm = s.get("Bcm", 0)
    print(
        "%d %d %d %.3f %d %d %.3f"
        % (
            ir,
            d1,
            s.get("DLmr", 0),
            (d1 * 100.0 / ir) if ir else 0,
            bc,
            bcm,
            (bcm * 100.0 / bc) if bc else 0,
        )
    )
