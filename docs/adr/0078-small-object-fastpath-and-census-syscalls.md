# 0078 — Frameless single-TLS free/calloc/realloc, C++17/C23 sized & aligned paths, and syscall-free census/first-span reuse

Accepted 2026-11-18.

## Evidence

Profiling `bench/instr`, `objdump -d libfastmalloc2.so`, and `cc1plus` (`g++ -O2 -c heavy.cpp`) under Callgrind, `/usr/bin/time -v`, and `strace -c -f` exposed two classes of avoidable overhead:

1. **Hot-path instruction, TLS, and prologue tax.**
   * `free()` executed 24–26 instructions, 8 branches, and 3 separate TLS base loads (`fm_head`, `fm_list_span`, `fm_cnt`) per small deallocation:
     * `fm_list_span[cls]` cost 6 instructions + 2 branches per free (1.41M Ir on `mini_heavy.cpp`) yet became dead after `ea2cf95` stopped walking flushed chains (`count == 0` batches are walked by `fm_trim_class` regardless of `FM_COUNT_PURE`, and `dead_mark` spans are always rebuilt with `count > 0 | FM_COUNT_PURE`);
     * `fm_tp_scan_module` checked only `0x48` (`REX.W`) and missed `0x4C` (`REX.WR`, `%r8..%r15`), leaving `free()`'s `fm_head` GOT load unpatched;
     * separate `if (!p)` and `((uintptr_t)p & 0xFFFF) == 0` tests were redundant because `NULL` also satisfies `(p & 0xFFFF) == 0`;
     * `fm_cnt[cls]` tested `cnt == 0` both before and after decrementing (6 instructions + 2 branches instead of `subl $1, %fs:disp(base,cls,4); jle`).
   * `calloc()` took `&zeroed` in the main body, triggering `-fstack-protector-strong` (`%fs:0x28` canary setup/check + stack frame + `%rbx` spill) and calling `memset@GOT` even on `<= 64 B` allocations (`cc1plus` called `calloc` ~133k times on `mini_heavy.cpp`).
   * `realloc()` inlined its entire move/grow slow path, spilling 6 callee-saved registers (`%rbx..%r15`) even when `size <= kClassSize[cls]` returned in place.
   * `FM_CFG()` called `pthread_once` via GOT on every slow-path lookup (`fm_flush_bin`, `fm_refill_class`, `fm_large_*`).
   * `aligned_alloc`/`posix_memalign` with `align` 32 or 64 mapped and trimmed a 64 KiB region (`mmap` + 2 `munmap`s + `munmap` on free = ~14.8 us/op), even though every small span is 64 KiB-aligned with a 64-byte header so rounded small classes are naturally 32B/64B-aligned; C++ `operator new`/`operator delete(void*, size_t)` and C23 `free_sized` were not exported.
2. **Kernel syscall and minor-fault tax on `g++ -O2 -c heavy.cpp`.**
   * `v2` made 2,457 memory syscalls and 42,203 minor faults vs `tcmalloc`'s 333 syscalls and 39,373 minor faults (+40 ms system time):
     * `FM_SMALL_FIRST_SPAN` bypassed `fm_map_dense_span()` and called `fm_map_aligned` (190 `mmap` + 342 `munmap` syscalls);
     * `fm_trim_class` `mmap`ped and `munmap`ped `FM_Census` (45 KiB) and `order` on every census (343 syscalls) and called `mincore` (223 calls) before checking whether `>= purge_min_pages` pages were structurally free;
     * `fm_tp_apply` called `mprotect` twice per patch across 36 KiB of `.text` (54 extra `mprotect` calls + 364k Ir/process);
     * `cc1plus` cycles ~20 `obstack` chunks of 64 KiB per function, exceeding `FM_LCACHE_HOT_WINDOW` (8) even after proving reuse (`fm_lcache_warm_mask`), causing 394 `munmap(135168)` + 462 `mmap(135168)` calls and ~3,000 minor faults.

## Decision

1. **Single-base `FM_HotTls` + 17-instruction frameless `free()`.** Group `head`, `cnt` (`int32_t`), and `tail` as parallel arrays inside one `__thread FM_HotTls fm_tls` (`head` at offset 0), remove `fm_list_span`, fold `NULL` into the low-16-bit alignment test, and collapse countdown decrement + lazy `.tbss` initialization into `subl $1, %fs:0x488(base,cls,4); jle fm_free_cnt_slow`. Keep `__builtin_prefetch(next, 0, 1)` on pop (`rand` 88.4 -> 69.6 ns/op in A/B probe).
2. **Frameless `calloc()` and `realloc()`, fast `FM_CFG()`.** Move `calloc`'s large/refill paths out of line (eliminating the stack canary), zero `<= 64 B` hits inline with 16-byte SSE stores, split `fm_realloc_slow` out of `realloc` (eliminating all 6 callee-saved spills on in-place `realloc`), and guard `pthread_once` in `FM_CFG()` with an acquire load of `fm_cfg_ready`.
3. **32B/64B small-span alignment + C++14/17 & C23 entry points.** Serve power-of-two `align <= 64, size <= SMALL_MAX` requests from small spans (`FM_ALIGN_UP(size, align)`), export C23 `free_sized`/`free_aligned_sized`, and export Itanium C++ `operator new`/`operator delete` (including sized `_ZdlPvm`/`_ZdaPvm` and `std::align_val_t` overloads, with lazy `dlsym` lookup of `std::get_new_handler` and `std::__throw_bad_alloc` on cold OOM).
4. **Syscall reductions.** Carve `FM_SMALL_FIRST_SPAN` 64 KiB slots from `fm_map_dense_span()` (`nblocks` still capped to 16 KiB so tail pages remain untouched), reuse a `fm_census_lock`-protected `FM_Census` table across censuses (unmapped on `malloc_trim`) with a 256-entry stack `order` buffer, gate `mincore` behind `cand_pages >= purge_min_pages`, widen `fm_large_free`'s hot window from 8 to 64 only for buckets already marked in `fm_lcache_warm_mask` (preserving cold teardown RSS in `bench/macro trim`), and scan only hot entry windows in `fm_tp_scan_module` (supporting `0x4C` `REX.WR` -> `0x49` `REX.WB` and one `mprotect` pair).

## Results

### Callgrind instructions/op (`bench/instr`, delta 20k->100k)

| mode | v2 before | v2 after | jit after | tcmalloc | mimalloc |
|---|---:|---:|---:|---:|---:|
| churn64 | 53.82 | **42.58** (-20.9%) | **41.61** | 81.00 | 112.98 |
| churn8 | 53.82 | **42.58** (-20.9%) | **41.61** | 81.00 | 105.24 |
| churn1024 | 59.05 | **47.12** (-20.2%) | **46.26** | 81.00 | 150.00 |
| churnmix | 65.92 | **54.65** (-17.1%) | 55.65 | 93.00 | 127.95 |
| mixed | 88.57 | **76.58** (-13.5%) | 77.54 | 115.01 | 93.11 |
| retain | 65.63 | **52.84** (-19.5%) | **52.53** | 86.00 | 60.76 |
| cross | 66.55 | **53.76** (-19.2%) | **53.32** | 87.00 | 62.00 |
| batch | 58.83 | **45.56** (-22.6%) | **44.60** | 84.03 | 59.74 |

### Microbenchmarks (`bench/micro`, `taskset -c 3`, min of 9 interleaved runs, ns/op)

| scenario | v2 before | v2 after | jit after | tcmalloc | mimalloc |
|---|---:|---:|---:|---:|---:|
| churn8 | 6.387 | **5.923** | 6.691 | 6.974 | 10.312 |
| churn | 6.335 | **5.669** | 6.680 | 6.696 | 11.060 |
| churn1024 | 6.581 | **6.228** | 6.975 | 6.368 | 14.309 |
| batch | 18.904 | **18.448** | 18.816 | 19.326 | 10.285 |
| batch1024 | 85.216 | **72.448** | **70.388** | 84.290 | 73.810 |
| rand | 111.153 | **72.222** | 73.389 | 155.941 | 66.799 |
| pareto | 3661.603 | **3587.925** | **3575.410** | 854.642 | 657.840 |
| hold | 22.253 | **21.748** | 23.007 | 22.337 | 13.442 |
| threads | 6.238 | **5.722** | 6.624 | 6.751 | 10.823 |
| cross | 42.171 | **34.070** | 41.999 | 102.410 | 43.244 |
| large | 14.132 | **12.308** | 13.645 | 6.568 | 18.179 |
| realloc | 3.954 | **3.000** | 3.130 | 4.419 | 4.682 |
| calloc | 660.322 | 667.183 | 663.367 | 624.667 | 671.709 |
| touch | 8.039 | **6.746** | 6.976 | 6.082 | 17.752 |

C++20 microbenchmarks (min of 7 interleaved, `taskset -c 3`): 24 B sized `new`/`delete` improved **11.029 -> 6.292 ns/op** (`tcmalloc` 7.438, `mimalloc` 11.601); 64 B `alignas(64)` `new`/`delete` improved **14,849.9 -> 8.5 ns/op**.

### Real C++ compilation (`g++ -O2 -c heavy.cpp`, 9 interleaved runs)

Memory syscalls fell **2,457 -> 707** (-71%) and minor faults fell **42,203 -> 38,190** (`tcmalloc` 39,385):
* **v2 before:** median 2.988 s (min 2.856 s), RSS 157.4 MiB
* **v2 after:** median **2.943 s** (min **2.845 s**), RSS 158.2 MiB — fastest median of all six allocators (`jemalloc` 3.012 s, `glibc` 3.051 s, `tcmalloc` 3.111 s / 161.9 MiB, `mimalloc` 3.211 s).

### Real-application suite (`ab.py`, `REPS=7`, median time / RSS)

| app | glibc | v2 before | v2 after | mimalloc | jemalloc | tcmalloc |
|---|---:|---:|---:|---:|---:|---:|
| jq | 1.405s / 264M | 1.212s / 258M | 1.227s / 258M | 1.173s / 272M | 1.251s / 277M | 1.247s / 274M |
| python | 0.388s / 141M | 0.398s / 143M | **0.386s / 143M** | 0.397s / 146M | 0.393s / 159M | 0.401s / 153M |
| sort | 0.130s / 29M | 0.122s / 30M | **0.118s / 30M** | 0.116s / 30M | 0.130s / 32M | 0.136s / 36M |
| xz | 8.277s / 92M | 7.820s / 93M | 8.061s / 93M | 8.724s / 107M | 8.951s / 96M | 7.831s / 112M |
| git | 0.013s / 7M | 0.015s / 8M | 0.015s / 8M | 0.014s / 8M | 0.017s / 10M | 0.021s / 14M |

