# Round 12 startup footprint

Run from the repository root. `work/` contains uncommitted builds; all retained
samples/logs are in `bench/results/round12-startup/`. Python 3, Node, GCC and the
normal battery prerequisites are required; no new package dependency.

1. At `3d1e114`, `make -B -j4`, then copy both v2 libraries to
   `work/round12/base/` (retain their ordinary filenames).
2. Generate `bench/apps/work/big.json` with the JSON fixture block in
   `bench/apps/prep.sh` (SHA256 in results/environment.txt).
3. `python3 bench/round12/startup.py RESULTS.jsonl` reproduces round 10's
   preloaded-wrapper method and a target-only preload control, seven rotated
   repetitions. Installed jemalloc is included; mimalloc is included if
   `work/libmimalloc.so` exists. It never silently ignores loader errors.
4. Build the candidate (final policy is static-only). To reproduce the rejected
   shared/JIT attempt, compile fm_jit.c + jit_stubs.S with the normal Makefile
   flags plus `-DFM_SMALL_FIRST_SPAN`, saving that experimental DSO separately;
   restore the ordinary JIT build before final verification. Add `--candidate` to startup.py to compare both builds
   in one serial session. `attribute.py RESULTS.jsonl` pauses Python/Node just
   after startup and classifies real span headers and resident pages through
   `/proc/PID/mem`, `pagemap` and `smaps`. Three repetitions, no allocator changes.
5. `bash bench/round12/diagnostic.sh` reuses round 10's diagnostic API accounting
   for live usable bytes, adds batch-page/census counters, and runs the same
   paused snapshots. Instrumented timings are never performance evidence.
6. `python3 bench/round12/guard.py RESULTS.jsonl` alternates seven repetitions
   of the actual Python/Node matrix workloads and 3M KV operations. `--cpu`
   uses wait4 directly and records CPU as well as wall time; `--reps N` changes
   the count. Both implementations and baseline DSOs run serially. App checksums
   must match the known fixture results.
7. `bash bench/round12/verify.sh` runs the all-class geometry/live-island/purge
   check, then `make` and the **entire** round7 suite in release and FM_DEBUG.
   It adds three intensified debug stress runs per implementation and restores
   the release build, including on failure. See individual logs, not just a
   successful final rebuild.

The timing driver must not inherit LD_PRELOAD/FM_* from the caller. Scripts
remove them and preload only the selected target (except the explicit round10
reproduction). No timing run overlaps another benchmark, diagnostic or build.
Snapshot startup snippets add a stop operation; they are not the exact no-op
commands and must not be substituted for the separate startup HWM measurements.
RSS components are from one synchronized snapshot, not an additive attribution
of the historical high-water mark. `g_exact` and central resident-page counts
are subsets of other categories and include any shared boundary page. Live
usable capacity and mapped bytes are not RSS. The residual contains runtime
heaps/stacks and unclassified aligned allocations; it is not all allocator
retention.

`python3 bench/round12/summarize.py` validates the retained sample counts,
startup range separation and battery completion logs, and writes summary.json.
It retains both timing passes and their pooled within-pass ratios; ADR 0061
explains why the JIT application was rejected despite overlapping ranges.
