#!/usr/bin/env python3
"""churn.py -- allocation-heavy Python workload.

Three sections, chosen with CPython's allocator layers in mind:
  1. json load/dump cycles over a large payload: the C json scanner
     builds hundreds of thousands of objects; small ones come from
     pymalloc arenas, large strings from raw malloc.
  2. dict/list churn of Python objects (pymalloc territory; the
     256 KiB arenas themselves still come from the interposed malloc).
  3. large bytes churn (16-256 KiB blocks): objects above pymalloc's
     512-byte ceiling go straight to libc malloc/realloc, so this
     section measures the interposed allocator directly.
Prints a checksum so corruption changes the output.
"""
import json
import sys

path = sys.argv[1]
acc = 0

with open(path, "rb") as f:
    raw = f.read()

for _ in range(3):
    data = json.loads(raw)
    acc += len(data)
    out = json.dumps(data)
    acc += len(out) % 1000

objs = []
for i in range(400000):
    objs.append({"id": i, "k": "v%d" % (i % 5000), "xs": [i, i + 1, i + 2]})
    if len(objs) > 8192:
        objs = objs[4096:]
acc += sum(o["id"] % 7 for o in objs[:1000])

blocks = []
for i in range(3000):
    b = bytes((i * 31 + j) % 251 for j in range(64)) * (256 * (1 + i % 4))
    blocks.append(b)
    if len(blocks) > 64:
        blocks.pop(0)
acc += sum(len(b) for b in blocks) % 100000

print("python-churn checksum", acc)
