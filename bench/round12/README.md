# Round 12 Lua peak — rejected prototype

The allocator diff is retained in `cold-purge.patch`, **not shipped in src/**.
It passes Lua's peak gate and correctness checks but fails to establish guard
neutrality (ADR 0058). All commands run from the repository root, based on
main `3d1e114`. Do not overlap timings with builds or correctness jobs.

Dependencies: Lua 5.4.7 (`lua5.4_5.4.7-1+b2_amd64.deb`), mimalloc 3.0.1
(`libmimalloc3_3.0.1+ds-2.1_amd64.deb`), system jemalloc, Python 3, GCC 14.2.
Debian pool packages were extracted under `work/tools` with `dpkg-deb -x`;
no system installation or dependency source changes are needed.

Baseline and resident decomposition, before applying the patch:

```
mkdir -p work/round12 bench/results/round12
make -j4
cp libfastmalloc2.so work/round12/base.so
python3 bench/round12/baseline.py
python3 bench/round10-shortlived/make_diag.py
cc -O3 -g -fPIC -fno-plt -Isrc -Iwork -shared work/fm_static_diag.c -o work/libdiag.so
python3 bench/round12/decompose.py
```

Prototype and initial comparisons:

```
git apply bench/round12/cold-purge.patch
make -j4
cp libfastmalloc2.so work/round12/cold.so
python3 bench/round12/compare.py
cc -O2 -pthread bench/ownership.c -o work/round12/ownership
cc -O2 bench/apps/bursty.c -o bench/apps/bursty
python3 bench/round12/guards.py > bench/results/round12/guards-summary.txt
bash bench/round12/verify.sh
```

The verification script runs complete round7 suites with the prototype off/on,
in ordinary and FM_DEBUG builds, then restores the ordinary prototype build.
This covers static/JIT, profiles, purge/churn stress, lifecycle fault injection,
fork, early init, TLS exit and retained epoch grace. Its focused internal test
checks cold payload discard, header survival, identical-address reuse, warm
retention, drain and calloc contents. For candidate diagnostics, run make_diag.py
again with the patch applied, compile its source to `work/round12/diag-cold.so`,
then run:

```
FM_LCACHE_COLD_PURGE=1 python3 bench/round12/decompose.py work/round12/diag-cold.so bench/results/round12/decomposition-cold.json
```

Run `churn2.lua` under base/cold three times each for the separate requested-live
phase probe. Diagnostic observations are not throughput measurements.

After the first guards raised a many-class concern, the following confirmation
was specified **before seeing its data**: ten rounds of all four churn cases,
rotating/reversing four labels. `base-repeat` uses exactly base's DSO path and
environment. `cold-off` uses candidate code with the option disabled. All
original measured samples are retained; confirmation does not replace them.

```
python3 bench/round12/churn-confirm.py > bench/results/round12/churn-confirm-summary.txt
python3 bench/round12/summarize.py
git apply -R bench/round12/cold-purge.patch
bash bench/round12/verify-final.sh
```

`verify-final.sh` validates the final unchanged production allocator with full
ordinary and FM_DEBUG batteries and restores an ordinary build. Recorded
prototype/final logs have separate directories. A final focused-test recheck
used a copy of the patched sources under `work/round12/prototype`.

Measurement details: exact app-matrix churn.lua command and run_timed.py
(wait4 HWM), checksum 19466078, bypassing the legacy best-of-N reducer. Five
rotated/alternating measured samples per configuration. Guards have an
additional excluded warmup pair. No failed/slow measured sample is dropped.
All comparisons set FM_LCACHE_COLD_PURGE=1 for both DSOs; main ignores it.
The baseline can alternatively be rebuilt from `git archive 3d1e114 src` with
the Makefile's O3 flags. Runtime dependencies are extracted in ignored work/.

The diagnostic sampler reuses the existing round-10 pagemap/header/live-capacity
probe, adapted to Lua and 10 ms sampling. Pausing/instrumenting changes timing:
sampled synchronized peaks are not exact decompositions of uninstrumented
historical HWM. Live counts are usable capacity, not requested bytes; partial
resident spans include live data and are not entirely returnable. These shared
cloud measurements do not establish controlled-hardware rankings. Reruns
replace named output files; archive prior evidence first.

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
7. `bash bench/round12/verify-startup.sh` runs the all-class geometry/live-island/purge
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

`python3 bench/round12/summarize-startup.py` validates the retained sample counts,
startup range separation and battery completion logs, and writes summary.json.
It retains both timing passes and their pooled within-pass ratios; ADR 0061
explains why the JIT application was rejected despite overlapping ranges.
