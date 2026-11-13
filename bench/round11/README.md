# Round 11 census contention experiments

Run from the repository root, sequentially (no builds/tests during timings):

```
python3 bench/round11/build.py
make -j4
python3 bench/round11/measure.py blocking trylock
python3 bench/round11/diagnose.py
python3 bench/round11/measure.py trylock spin
python3 bench/round11/measure.py trylock hint
python3 bench/round11/measure.py trylock backoff
cp work/round11/trylock.so work/round11/identical.so
python3 bench/round11/measure.py trylock identical cross
cp work/round11/blocking.so work/round11/blocking-no-census.so
cp work/round11/trylock.so work/round11/trylock-no-census.so
FM_RELEASE_MIN_DEPTH=65536 python3 bench/round11/measure.py blocking-no-census trylock-no-census cross
FM_RELEASE_MIN_DEPTH=65536 python3 bench/round11/diagnose.py no-census-diagnostics
bash bench/round11/verify.sh
```

The isolated sources and DSOs are generated in ignored `work/round11`.
Sources are extracted from immutable main e598f28. Production sources are
unchanged unless a candidate meets the adoption gate.
Each series excludes one warmup pair and retains five alternating measured
pairs per case. Ranges include all measured samples; no best-of-N selection.
Cases/operation counts match round 10. Cross uses two threads; the final `8`
argument only affects local mode. Many/census assert 800,000 ownership transfers.
Times include subprocess startup/exit. Diagnostic counters are separate builds,
never inputs to throughput claims. The diagnostic destructor aggregates census
attempts, acquisitions, skips, blocks walked, bytes unmapped/purged, and small
span creation requests. wait4 also records CPU time, context switches and raw
high-water RSS (which can inherit launcher HWM and is not an allocator RSS
comparison). No timing claim uses instrumented samples. The forced-busy
diagnostic self-check is compiled and run by build.py. The optional diagnostic
output label prevents overwriting the original series. The no-census control
uses the existing runtime depth gate; diagnostic counters verify zero attempts.

Results go to `bench/results/round11-crossfree`; reruns overwrite named series,
so preserve prior evidence before rerunning. These are shared cloud timings.
The spin prototype uses x86 `pause` and is an isolated Linux/x86 experiment.
The hint is advisory: all paths still lock, even when its relaxed load is stale.
Backoff skips the next three automatic opportunities after a failed trylock.
Explicit trim always blocks and no prototype changes central locking, atomic
`dead_mark`, span geometry, or fork lock ordering.
