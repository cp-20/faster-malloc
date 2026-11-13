# Round 9 sanitizer evidence

See [Sanitizer proof](../round9-2026-11-13.md#sanitizer-proof) for flags,
coverage boundaries, results and triage. The matching reproducible runners are
in `bench/sanitizers/`; they write logs to the ignored `work/sanitizers/` tree.

- `compiler.txt`: compiler, kernel and glibc versions.
- `interposition-results.json`, `interposition.log`: exact round7 battery,
  independent cases after failures, and FM_DEBUG stress. UBSan passes; ASan/TSan
  startup and DEEPBIND failures are intentional retained limitations.
- `namespaced-results.json`, `namespaced.log`: instrumented allocator main
  matrix. Four initial mt cross-free exit-66 reports are superseded by the
  fully instrumented client recheck below, not suppressed.
- `mt-recheck-results.json`, `mt-recheck.log`: fully instrumented release/acquire
  handoff, 24/24 local/cross-free runtime repetitions pass across both allocators
  and TSan / TSan+UBSan; all eight compilation steps also pass.
- `lifecycle-results.json`, `lifecycle.log`: final matrix after the fault-counter
  fix, 144/144 build/run steps pass (72 runtime cases).
- `early-recheck-results.json`, `early-recheck.log`, `early-init-order.log`:
  32/32 steps pass, including eight constructor-order-preserving earlyinit runs.
  Runtime-first plus namespaced-allocator preload restores original ordering;
  the loader trace confirms the dependency initializes before the allocator.
  These supersede the initial simply-linked earlyinit cases in lifecycle logs.
- `probes-results.json`, `probes.log`: load-order ownership, sanitizer combination
  rejection, helper-thread control, POSIX output-pointer reproduction and fully
  instrumented before/after JIT race reproduction. Nonzero statuses in the
  *before* and incompatible-runtime controls are expected evidence.
- `posix-before-results.json`: initial namespaced v2 ASan basic cases; all four
  profiles fail before ADR 0044.
- `triage-before.log`: original fault-counter race, cross-free false positives,
  thread-count assertion, one fully instrumented TSan stress success, initial
  three-warning JIT reproduction and clean fixed combination controls.
- `baseline.log`, `baseline.status`: final normal correctness battery, exit 0.

Exit status -11 means SIGSEGV; -6 means SIGABRT; 66 is TSan's diagnostic exit.
Shell scripts may turn a signal into 128+signal. Timings are wall times on a
shared host, not performance measurements. Historical workspace paths and PIDs
in diagnostics are retained verbatim for traceability.
