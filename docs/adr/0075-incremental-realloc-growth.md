# 0075 — Reserve capacity for incremental realloc growth

Accepted 2026-11-18.

## Evidence

On CPython 3.12.3, the record arrays, JSON writer and grouping lists in
`python_data` grow by small increments. An instrumented build counted 3,920
moves in 3,935 non-null small-span reallocs requesting 512 B to under 8 KiB.
Those moves copied 10.32 MiB. Another 34 moves from plain large mappings
copied 42.36 MiB. Phase counters put 9,833 minor faults in JSON serialization
alone. The host's Python also mapped 109 one-MiB regions directly.

## Decision

For a growing medium buffer (old capacity at least 512 B), round the next
sub-doubling request to a power-of-two class within the 32 KiB small-span
limit. For plain large buffers, first use existing capacity or look for a
cached mapping sized to the request. On a miss during sub-doubling growth,
reserve up to one extra request's worth of virtual memory, capped at 8 MiB
before rounding. Round reservations of at least 1 MiB to a multiple of 2 MiB
and hint transparent huge pages over the requested range for requests of at
least 1 MiB. Keep the requested length in the large header. Both paths retry
the exact request if reserving extra capacity fails.

The same diagnostic counted 923 medium moves (1.63 MiB) and 13 large moves
(8.37 MiB) after the change. Python minor faults fell from 47,249 to 35,624.
`strace -c -f` counted 611 → 423 mmap, 1,019 → 587 munmap and 30 → 25
madvise calls. Both builds made one brk call. This decision affects the
shared realloc path and preserves the large-cache search and purge policies.

## Workload measurements

Unpinned, interleaved processes on x86-64 WSL2, default allocator settings,
baseline `22d1694`. Times are minimum and median wall seconds. RSS is the
median of per-process peak KiB from `/usr/bin/time -v`.

| Workload | Runs/build | Before min / median | After min / median | Before RSS | After RSS |
|---|---:|---:|---:|---:|---:|
| python_data | 3 | 0.4400 / 0.4618 | 0.3765 / 0.3984 | 154,084 | 147,124 |
| jq_large | 12 | 1.4338 / 1.5760 | 1.4293 / 1.5769 | 258,696 | 258,754 |
| sort | 12 | 0.1194 / 0.1320 | 0.1130 / 0.1225 | 31,032 | 31,032 |

Python minima for mimalloc 2.1.2, jemalloc 5.3.0 and glibc were 0.3790,
0.3748 and 0.3823 s. A separate ten-round Python comparison gave median
times of 0.4113 s before, 0.3903 s after and 0.3920 s for mimalloc. The
v2–mimalloc difference is within run-to-run variation. The jq and sort rows
include nine follow-up pairs because the first three jq minima differed by
6% while the instrumented jq made just one measured medium move and zero
large realloc calls on either build.

Both v2 and JIT passed the incremental growth test, `bench/check` and
`bench/stress`. Under a constrained `RLIMIT_AS`, a 4 MiB reservation failed,
the exact 1.5 MiB request succeeded and the original contents survived.

## Amendment — keep the hit paths free (2026-11-18)

The first version passed the grow-from size as an extra argument through
`fm_large_alloc_search` and lost the in-place realloc branch layout, costing
`micro large` about +11 instructions/op and `micro realloc` about +13% wall time.
The miss path now lives in a cold `fm_large_alloc_map`, the old size travels in
the thread-local `fm_grow_from` (set and cleared by realloc only), and the
in-place check is marked likely. Both micros are now at or below main.
