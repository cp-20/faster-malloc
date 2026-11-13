# Real-application matrix (rounds 3–5)

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

## Round-5 cells

Four more use-case shapes, each a standalone script on the
shared `cellib.sh` conventions (same seven allocators, min-of-N,
peak RSS, correctness gate). They are not part of `apps.sh`;
run them individually:

```sh
REPS=3 bench/apps/nginx.sh     # web server under HTTP load
REPS=3 bench/apps/ruby.sh      # Ruby workload (bench.rb)
REPS=3 bench/apps/bursty.sh    # bursty long-running server heap
PGREPS=2 bench/apps/pgbench.sh # PostgreSQL OLTP
```

| cell    | what runs | what it adds |
|---------|-----------|--------------|
| nginx   | nginx (2 workers) serving static files under `wrk.c` keep-alive load (20 conns, 10 s); reports rps + p50/p99 latency + summed peak RSS; fetched body diffed, wrk must report 0 errors | a real network server: per-request small allocations across processes, latency percentiles, not just throughput |
| ruby    | `bench.rb`: string/array/hash churn with a checksum | a third managed runtime, with a different GC/allocator interaction than CPython (pymalloc) and V8 |
| pgbench | PostgreSQL 16, pgbench scale 5, 4 clients / 2 jobs, 15 s; server runs as `nobody`, `LD_PRELOAD` crosses via the environment; 0 failed transactions required | a multi-process OLTP database — a second DB shape next to redis (single-threaded) and sqlite (embedded) |
| bursty  | `bursty.c`: a server-shaped heap that ramps to ~60 MB, idles, ramps to ~90 MB, cohort-drains to ~10 MB and idles again; op-counted phases, so the work — and the checksum — is identical for every allocator | the long-running daemon shape: whether memory freed at drain time actually leaves during the idle that follows (ADR 0020/0024 in a realistic pattern). The columns that matter are the idle-phase RSS values |

nginx harness note: nginx closes a keep-alive connection after
1000 requests by default (`keepalive_requests`), and `wrk.c`
drives ~3500 requests per connection — the generated config
raises the cap so the cell measures the allocator, not nginx's
connection policy. The script also kills stale `fm-nginx`
masters from aborted runs before starting (a leftover master
holds the port and serves a deleted docroot).

## Provisioning on this host

Fixtures (sqlite amalgamation + speedtest1, `big.json`, the git
repo) are built by `prep.sh` into `work/`.

Binaries not on a stock system were installed as distro
packages: `redis-server`/`redis-tools` 7.0.15, `lua5.4` 5.4.6,
and the comparison allocators `libjemalloc2` 5.3.0,
`libmimalloc2.0` 2.1.2, `libtcmalloc-minimal4t64` 2.15
(Ubuntu 24.04 packages). Round 5 added `nginx` 1.24,
`postgresql-16` (+ `pgbench`) and `ruby` (distro packages);
`wrk` and `bursty` are built from the `.c` sources here by
`prep.sh`. `apps.sh` also honours `REDIS_BIN`,
`REDIS_BENCH`, `REDIS_CLI` and `LUA_BIN` overrides pointing at
source builds (`/tmp/r3tools/...` by default as a fallback).

Timing uses `run_timed.py` (fork/wait4 + rusage) instead of GNU
time so the harness has no extra dependency; for redis, whose
server daemonises, the wall time brackets redis-benchmark and
RSS is the server's VmHWM.

## Round-11 offline data tools

Two additional standalone matrix cells use installed GNU sort and XZ, with
no downloads or builds at runtime:

```sh
make
MIMALLOC_LIB=/path/to/libmimalloc.so.2 REPS=5 python3 bench/apps/data_tools.py
```

`data_tools.py` creates a deterministic 400,000-row TSV event log, then runs
single-threaded numeric-key sorting (`sort --parallel=1 -S 128M -k1,1n`) and
log compression (`xz -T1 -6 -c`). It requires glibc, v2, jemalloc and mimalloc
rather than silently skipping a comparison. `JEMALLOC_LIB` and `MIMALLOC_LIB`
override library paths. Allocator profile variables are cleared, so v2 is
balanced. Each repetition rotates the first allocator. A libc warmup is
checked with `sort -c` or XZ decompression and a SHA-256 round trip; every
measured output must match its reference hash. Failures stop the run.

Unlike the older minimum-time cells, this reports separate medians and
min/max ranges for wall seconds and peak RSS (KiB), plus all raw samples in
`bench/apps/work/data-tools.json` (`RESULTS` overrides the destination).
Timing reuses `run_timed.py` with the preload applied only to the child tool;
fixture generation, reference validation, and hashing are outside timing.
These measure warm-cache local data processing, not disk throughput or
multithreaded compression. At least five repetitions are mandatory.
