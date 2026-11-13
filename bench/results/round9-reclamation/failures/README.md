# Retained validation failures

The first `bash bench/round9/verify.sh` stopped with SIGBUS in JIT
`FM_PROFILE=footprint ./test_basic`, after v2 completed its checks.
`locked-battery-first.txt` retains the complete transcript. At this point the
normal v2 and JIT `.text` sections matched builds of untouched d9c4364
(hashes in ../host.txt). No epoch define was enabled in that failed DSO.

Diagnostic command, five repetitions of each profile:

```sh
for rep in 1 2 3 4 5; do
  for profile in balanced throughput footprint; do
    echo "$rep $profile"
    FM_PROFILE=$profile LD_PRELOAD=$PWD/work/reclamation/original-jit.so ./test_basic
  done
done
```

All 15 passed; see `original-jit-profiles.txt`. This does not explain or fix
the failure. A separate full verify rerun is recorded outside this directory;
its eventual passing status must not erase this earlier failure.

The earlier helper-control bursty SIGSEGV is separately retained under
../preliminary/. Neither failure is attributed to the epoch prototype or
claimed resolved by a rerun. No failed sample contributes a throughput median.
