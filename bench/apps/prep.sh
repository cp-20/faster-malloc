#!/bin/bash
# prep.sh -- build the fixtures for the real-application matrix.
#
# Everything lands in bench/apps/work/ (gitignored): the sqlite
# amalgamation + speedtest1, a large JSON document, and a synthetic
# git repository.  Tool binaries that apps.sh looks for on PATH or in
# well-known scratch locations (redis, lua) are NOT built here; see
# bench/apps/README.md for how they were provisioned on this host.
set -eu
cd "$(dirname "$0")"
WORK=$PWD/work
mkdir -p "$WORK"
cd "$WORK"

SD=sqlite-amalgamation-3450100
if [ ! -x speedtest1 ]; then
    if [ ! -d $SD ]; then
        curl -sL -o sqlite.zip \
            https://www.sqlite.org/2024/sqlite-amalgamation-3450100.zip
        python3 -c "import zipfile; zipfile.ZipFile('sqlite.zip').extractall()"
        rm -f sqlite.zip
    fi
    curl -sL -o speedtest1.c \
        https://raw.githubusercontent.com/sqlite/sqlite/master/test/speedtest1.c
    gcc -O2 -I$SD -DSQLITE_THREADSAFE=0 -o speedtest1 speedtest1.c \
        $SD/sqlite3.c -lm
    echo "built speedtest1"
fi

if [ ! -f big.json ]; then
    python3 - <<'EOF'
import json, random
random.seed(42)
data = [{
    "id": i,
    "name": "item-%d" % (i % 10000),
    "v": random.randrange(100000),
    "price": random.randrange(100, 10000) / 100.0,
    "tags": ["t%d" % (i % 97), "t%d" % (i % 89)],
    "nested": {"a": i % 13, "b": "x" * (i % 40)},
} for i in range(200000)]
with open("big.json", "w") as f:
    json.dump(data, f)
print("wrote big.json")
EOF
fi

if [ ! -d gitfix ]; then
    mkdir gitfix && cd gitfix
    git init -q -b main .
    # 1500 commits x (2 small file edits + occasional 64 KiB blob):
    # enough history for log/rev-list/gc to do real allocation work,
    # small enough to rebuild in seconds via fast-import.
    python3 - <<'EOF' | git fast-import --quiet
import random
random.seed(7)
print("reset refs/heads/main")
files = {}
for c in range(1500):
    print("commit refs/heads/main")
    print("mark :%d" % (c + 1))
    print("author Bench <bench@example.com> 1700000000 +0000")
    print("committer Bench <bench@example.com> 1700000000 +0000")
    msg = "commit %d" % c
    print("data %d" % len(msg)); print(msg)
    if c: print("from :%d" % c)
    for k in range(3):
        name = "dir%d/file%d.txt" % (k, (c + k) % 50)
        body = ("content %d %d\n" % (c, k)) * 20
        print("M 100644 inline %s" % name)
        print("data %d" % len(body)); print(body)
    if c % 25 == 0:
        blob = bytes(random.randrange(65, 91) for _ in range(65536))
        print("M 100644 inline blob%d.bin" % (c // 25))
        print("data %d" % len(blob))
        import sys
        sys.stdout.flush()
        sys.stdout.buffer.write(blob + b"\n")
print("done")
EOF
    cd ..
    echo "built gitfix"
fi
# Round-5 cell binaries (bench/apps/*.c) live next to this script,
# not in work/: wrk (nginx load generator) and bursty (server-shaped
# heap).  Binaries are gitignored; build them here so the cells
# are reproducible from a fresh checkout.
cd "$WORK/.."
gcc -O2 -pthread -o wrk wrk.c
gcc -O2 -o bursty bursty.c
echo "built wrk, bursty"

echo "prep done: $(du -sh $WORK | cut -f1) in $WORK"
