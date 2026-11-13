# Short-lived RSS measurements

Run from the repository root. `make`, the app fixture `bench/apps/work/big.json`
(the deterministic JSON generator in `bench/apps/prep.sh`), Node, Python and
jemalloc must be available. Build mimalloc 2.1.7 into `work/libmimalloc.so`:

```sh
mkdir -p work bench/results/round10-shortlived
curl -fL https://github.com/microsoft/mimalloc/archive/refs/tags/v2.1.7.tar.gz -o work/mimalloc.tar.gz
tar -xzf work/mimalloc.tar.gz -C work
gcc -O3 -shared -fPIC -pthread -Iwork/mimalloc-2.1.7/include \
  -Iwork/mimalloc-2.1.7/src -DMI_SHARED_LIB -DMI_SHARED_LIB_EXPORT \
  -o work/libmimalloc.so work/mimalloc-2.1.7/src/static.c
python3 bench/round10-shortlived/matrix.py
python3 bench/round10-shortlived/make_diag.py
gcc -O3 -g -shared -fPIC -fno-plt -Isrc -Iwork -Wl,-z,nodelete \
  -o work/libdiag.so work/fm_static_diag.c
python3 bench/round10-shortlived/decompose.py
bash bench/round7.sh
gcc -O2 -o bench/apps/bursty bench/apps/bursty.c
python3 bench/round10-shortlived/probes.py
python3 bench/round10-shortlived/startup.py
python3 bench/round10-shortlived/summary.py
```

Run these serially on an otherwise idle container; scripts append matrix and
decomposition data, so archive previous output first. The matrix invokes the
existing app workloads and `run_timed.py` once per sample (five rotated rounds),
retaining every checksum, exit status, duration and RSS. It avoids apps.sh's
fastest-run reducer. The eager configuration combines footprint with zero
release age and zero census interval floor/cap; knob clamps still apply.

The diagnostic library is **not a production allocator**. Its generated wrappers
count live usable allocation capacity and reuse small spans' otherwise-unused
`user_size` field for live capacity. Cached large headers get a zero user-size
marker. A parent stops the process between 50 ms intervals, reads resident span
headers through `/proc/PID/mem`, and counts resident pages with pagemap. It skips
nonresident header pages before reading to avoid faulting them in. Large aligned
headers outside the 64 KiB boundary are not attributed and remain in `other`.
The highest sampled RSS snapshot is retained. This is a synchronized sampled
breakdown, not an exact decomposition of wait4's historical HWM. Pausing and
instrumentation change timing; no diagnostic timings enter the matrix.

`fully_free_small_resident_kb` is resident small-span memory with no live API
allocations, including headers and local/central free blocks. `partial` includes
both live and free space in pinned spans: live usable bytes are capacity, **not
RSS**, and cannot be subtracted to give exact resident fragmentation. `other`
includes Python pymalloc/V8 direct mappings, code, stacks and allocator metadata;
it must not be described as allocator retention or entirely live memory. `baseline_mappings_resident_kb` splits out file-backed mappings, the named main
stack and kernel mappings at the same instant. Anonymous thread stacks and
allocator metadata remain in `other_anonymous_resident_kb` alongside the runtime
heaps; these are not a live-byte measurement. Startup
HWM is measured separately and is not subtracted from a different instant's RSS.

The exploratory files are retained for audit only: the initial matrix overlapped
with diagnostics/build activity. The final matrix and diagnostics ran serially.
