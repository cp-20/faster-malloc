# Real-application matrix (round 3)

`apps.sh` runs real programs under each allocator via
`LD_PRELOAD` and reports wall time (min of REPS, allocators
interleaved round-robin so host drift hits everyone) and peak
RSS. An app that crashes, hangs or changes its answer under an
allocator is a **FAIL with its exit status** — never silently
skipped, never back-filled with another allocator's numbers.

```sh
bench/apps/prep.sh          # once: build fixtures in work/ (gitignored)
REPS=3 bench/apps/apps.sh   # the full matrix
APPS="lua jq" REPS=1 bench/apps/apps.sh   # subset
```

Allocators: glibc, v1, v2, jit, plus jemalloc / mimalloc /
tcmalloc when their shared libraries are installed (found via
`ldconfig`). `ALLOCATORS=` overrides the list.

## The applications

| app    | what runs | runtime notes |
|--------|-----------|---------------|
| redis  | redis-server + redis-benchmark `-t set,get -n 100000 -c 16`, then a set/get round-trip check | pure C; the server itself must survive under the allocator |
| sqlite | `speedtest1 --size 30` against a fresh database | C library + shell built from the official amalgamation |
| jq     | filter/sort over a 200k-record JSON document (~40 MB) | C |
| lua    | `workloads/churn.lua`: string growth (realloc path), table churn, sort | Lua routes *every* allocation through one realloc-backed function, so this is the interposed allocator measured directly |
| python | `workloads/churn.py`: json cycles, object churn, large-bytes churn | CPython's pymalloc serves objects ≤ 512 B itself; only its 256 KiB arenas and larger objects reach libc malloc — read python rows with that in mind |
| node   | `workloads/churn.js`: JSON.parse/stringify, Buffer churn, object churn | V8 has its own heap/GC; the workload leans on the parts that reach libc (external strings, Buffers > 8 KiB pool) |
| gcc    | `gcc -O1 -c` on the SQLite amalgamation (`sqlite3.c`, ~250k lines) | real compiler, mostly cc1 |
| git    | `log`, `rev-list --objects --all`, `gc` on a synthetic 1500-commit repo | real everyday C tool |

Every language workload prints a checksum; a corrupted heap
changes the answer, not just the timing.

## Provisioning on this host

Fixtures (sqlite amalgamation + speedtest1, `big.json`, the git
repo) are built by `prep.sh` into `work/`.

Binaries not on a stock system were installed as distro
packages: `redis-server`/`redis-tools` 7.0.15, `lua5.4` 5.4.6,
and the comparison allocators `libjemalloc2` 5.3.0,
`libmimalloc2.0` 2.1.2, `libtcmalloc-minimal4t64` 2.15
(Ubuntu 24.04 packages). `apps.sh` also honours `REDIS_BIN`,
`REDIS_BENCH`, `REDIS_CLI` and `LUA_BIN` overrides pointing at
source builds (`/tmp/r3tools/...` by default as a fallback).

Timing uses `run_timed.py` (fork/wait4 + rusage) instead of GNU
time so the harness has no extra dependency; for redis, whose
server daemonises, the wall time brackets redis-benchmark and
RSS is the server's VmHWM.
