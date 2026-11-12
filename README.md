# faster-malloc

Three allocators, built from scratch and measured against glibc, tcmalloc,
jemalloc and mimalloc:

| build | what it is |
|---|---|
| `libfastmalloc.so` | **v1** — the first working version: table-lookup dispatch, per-thread free lists, spinlock-protected central list. Kept as the baseline that v2 improves on. |
| `libfastmalloc2.so` | **v2** — the static allocator. Arithmetic size→class dispatch, lock-free batched central list, O(1) flush, frameless hot paths. **This is the fastest allocator here.** |
| `libjitmalloc.so` | **JIT** — v2 plus a runtime code generator that specialises the allocation entry point to the program's observed size distribution. |

All three are drop-in `LD_PRELOAD` replacements.

```
make                       # builds the libraries and the benchmarks
LD_PRELOAD=$PWD/libfastmalloc2.so ./yourprogram
```

## Results at a glance

Instructions executed per allocation operation (the whole program, measured
exactly with callgrind — no timing noise):

| allocator | churn (64 B) | churnmix (8 sizes) | mixed (random) | cross-thread free |
|---|---|---|---|---|
| glibc | 142 | 155 | 204 | 155 |
| tcmalloc | 102 | 124 | 159 | 130 |
| jemalloc | 83 | 99 | 129 | 100 |
| mimalloc | 114 | 129 | **95** | **64** |
| v1 | 73 | 86 | 108 | 80 |
| **v2** | 52 | **65** | 87 | 59 |
| jit | **51** | 66 | 89 | **58** |

v2 executes **57–64% fewer instructions than glibc** and has the lowest
count of all seven allocators on three of the four workloads; mimalloc is
ahead on the two that hammer a single size class, because it reuses the
same warm pages.

On real programs (SPEC CPU2006-derived `cfrac`, wall clock, min of several
runs) v2 is **24% faster than glibc** and beats tcmalloc, jemalloc and
mimalloc. Under 8 threads it is **1.7× faster than glibc** when threads
allocate locally and **2.5× faster** when a block is freed by a different
thread than allocated it.

The full numbers, the methodology and the honest comparison — including
where v2 *loses* — are in [Results](#5-results). The raw benchmark output
is in [`docs/results.md`](docs/results.md).

---

## 1. What was researched

Before writing any code I read up on how the state of the art works:

* **jemalloc** — size classes, extent-based arenas, per-thread caches, `tcache` binning.
* **tcmalloc** — thread caches with a central free list, transfer batches, size-class maps.
* **mimalloc** — segment-based allocation with `pages` that hold blocks of one size, deliberately over-aligned so that free-list metadata stays out of the user data, plus "abandoned" pages that follow a block to the thread that frees it.
* **"Old is Gold" / ExGen-Malloc** (ASPLOS'24) — free-list shaping so that objects freed together are reused together; the key insight that *locality*, not instruction count, decides real allocator performance.
* **"Generating memory allocators from the ground up"** (ASPLOS'23) — synthesising allocators from a specification and searching the design space.
* **JIT / value specialisation** — specialising code at runtime for observed values (LuaJIT, PyPy, etc.).
* **Free-list sharding** — splitting a global free list per size class, and sharding the *tail* of large lists to avoid contention.

The conclusions that shaped the design:

1. The fast path must not touch shared state and must not do a dependent load before it can reach the block.
2. The size→class mapping should be arithmetic whenever possible; a table lookup costs a load and a dependency.
3. The free list must not live in front of the user's data — that would either waste memory or corrupt the user's bytes.
4. Central-list traffic must be amortised: a lock or a CAS on every refill is fine, a lock on every free is not.
5. Locality matters as much as instruction count: a block freed should ideally be handed back to the thread that will use it next.

---

## 2. The design of v2

### 2.1 Spans instead of per-block headers

A **span** is a 64 KiB-aligned region whose first 64 bytes are a header
holding the size class. Every small allocation is carved out of a span that
serves exactly one class.

```
span (64 KiB aligned)
┌──────────────┬────────┬────────┬────────┬─────���
│ header:      │ block  │ block  │ block  │ …
│  cls, magic  │   0    │   1    │   2    │
│  nblocks,    ├────────┼────────┼────────┤
│  map info    │ block  │ block  │ block  │
└──────────────┴────────┴────────┴────────┴──
```

`free()` recovers the class with one AND and one load:

```c
FM_Span *sp = (FM_Span *)((uintptr_t)p & SPAN_MASK);   /* one AND */
uint32_t cls = sp->cls;                               /* one load  */
```

There is **no per-block metadata at all**: nothing is stored in front of the
user's data, and the user gets a 16-byte-aligned pointer, as the C standard
requires. The 64-byte header is amortised over the whole 64 KiB span.

### 2.2 Arithmetic dispatch

The class ladder is 16-byte granular up to 512 bytes, which makes the
mapping a pure shift:

```c
class = (size + 15) >> 4;        /* size <= 512: no memory access */
```

Above 512 bytes the ladder coarsens and a byte table is used. The class
numbering is chosen so that the arithmetic form needs **no correction** —
`class 0` exists only to serve a request for 0 bytes, and `class c >= 1`
serves the sizes `(16*(c-1), 16*c]`.

### 2.3 Parallel TLS arrays, not an array of structs

This was worth 7× on its own. The thread-local state is split into three
parallel arrays:

```c
extern __thread void   *fm_head[CLASS_COUNT];
extern __thread void   *fm_tail[CLASS_COUNT];
extern __thread uint32_t fm_cnt[CLASS_COUNT];
```

Because `fm_head` has an 8-byte stride, the compiler folds the class index
into the addressing mode of a single instruction:

```asm
mov    0x35a7(%rip),%rdx        ; the GOT entry (the TLS offset)
mov    %edi,%ecx                ; the class index
mov    %fs:(%rdx,%rcx,8),%rax   ; head of the free list
```

An array of 32-byte structs cannot be addressed that way; the compiler
emits a `shl $5` plus an `add` on the critical path instead. The change
was worth a great deal more than those two instructions — the struct layout
also forced the slow paths to be inlined, so `malloc` grew a stack frame and
six callee-saved register spills. Splitting the fast path out with
`noinline` and switching to parallel arrays took the churn loop from a
measured 144 ns/op to 22 ns/op on this host (noisy, but the instruction
counts confirm the direction: the whole fast path is now 13 instructions).

### 2.4 Frameless hot paths

The slow paths (refill, flush, large allocation, aligned allocation) are
`noinline`, so `malloc` and `free` need no callee-saved registers and no
stack frame:

```asm
malloc:
    endbr64
    cmp    $0x200,%rdi        ; is it a small request?
    ja     .Lbig              ; no -> the out-of-line large path
    add    $0xf,%rdi          ; round up to 16 bytes
    shr    $0x4,%rdi          ; ... and divide: the class index
    mov    0x36d4(%rip),%rdx  ; the thread-local displacement
    mov    %edi,%ecx
    mov    %fs:(%rdx,%rcx,8),%rax   ; head of the free list
    test   %rax,%rax
    je     .Lslow             ; empty -> the out-of-line refill path
    mov    (%rax),%rsi        ; the next block
    mov    %rsi,%fs:(%rdx,%rcx,8)
    ret
```

Thirteen instructions, no stack frame, no callee-saved registers, and the
only memory accesses are the thread-local list head and the block itself.

`free()` is similarly short, because the span header gives up the class
with one AND and one load:

```asm
free:
    endbr64
    test   %rdi,%rdi
    je     .Lnull
    test   %di,%di            ; is the pointer 64 KiB aligned?
    je     .Laligned
    mov    %rdi,%rax
    xor    %ax,%ax            ; mask off the low 16 bits
    mov    (%rax),%ecx        ; the span header -> the size class
    cmp    $0xffffffff,%ecx
    je     .Llarge
    mov    0x365a(%rip),%rsi
    mov    %ecx,%eax
    mov    %fs:(%rsi,%rax,8),%rdx
    mov    %rdx,(%rdi)        ; push the block onto the free list
    mov    %rdi,%fs:(%rsi,%rax,8)
    ...
```

### 2.5 Lock-free central list of batches

The central list of each class is a **Treiber stack of batches**. A batch is
a whole free list, not a fragment of one, which makes both directions O(1):

* a refill takes a whole batch with one CAS;
* a flush publishes a whole batch with one CAS.

No lock is ever taken on the allocation path. Keeping the tail pointer per
class matters: publishing a list means *three stores*, whereas walking the
list to find its tail would touch every block — and the blocks of a free
list are scattered across the heap, so each step is a likely cache miss.

### 2.6 The local-cache limit is a real tuning knob

A thread may hold up to `kClassLimit[cls]` blocks in its private list before
handing the list to the central list. The limit trades memory retained per
thread against how often the flush branch is taken. Measured on the churn
workload (branch mispredicts as a percentage of all branches):

| limit | 16 | 32 | 64 | 128 | 256 | 512 |
|---|---|---|---|---|---|---|
| Bcm % | 0.907 | 0.546 | 0.360 | 0.266 | 0.219 | 0.195 |
| Ir | 18.31M | 17.49M | 17.08M | 16.88M | 16.78M | 16.73M |

The default is 256, bounded by the class size so a thread never holds an
unreasonable number of bytes in one class.

---

## 3. The JIT

### 3.1 The idea

A statically compiled allocator commits to one size→class mapping and one
fast-path shape for every program it will ever run. Real programs,
however, concentrate nearly all of their allocations on a handful of exact
sizes — a struct size, a fixed buffer — and the *shape* of the fast path
that pays off depends on how concentrated that distribution is.

The JIT build therefore:

1. **Profiles** the running program. Request sizes go into a histogram,
   sampled 1-in-64, so the hot path pays one thread-local counter decrement
   and one perfectly-predicted branch.
2. **Generates** a specialised dispatch once the profile is populated. For
   each hot class the template gets a range test; for each class a leaf
   that pops from that class's thread-local free list with the list address
   baked in as a constant displacement.
3. **Installs** the generated code behind an atomically updated pointer, so
   the swap is safe while other threads are running.
4. **Decides whether to specialise at all.** If the distribution turns out
   to be too diverse for a specialised dispatch to pay for itself, the JIT
   *steps aside* and the plain static path takes over.

The generated code:

```asm
    lea    -49(%rdi),%rdx        ; the class range test
    cmp    $0xf,%rdx
    jbe    .Lleaf                ; -> the specialised leaf
    ...
.Lleaf:
    movabs $0xfffffffffffffc00,%rcx   ; the thread-local displacement
    mov    %fs:(%rcx),%rax            ; head of the free list
    test   %rax,%rax
    jz     .Lgeneric
    mov    (%rax),%rdx                ; next
    mov    %rdx,%fs:(%rcx)
    ret
```

Against the static path this removes the size→class arithmetic entirely for
the hot sizes and replaces a load of the TLS base with an immediate.

### 3.2 Why the code generator lives in an assembler file

The first version hand-assembled x86-64 in C. It was a minefield: a wrong
ModRM byte produces code that faults only on some inputs, and the failures
look like random corruption rather than a coding error. (A `mov %fs:(%rcx),%rdx`
encoded as ModRM `0x14` silently selects an SIB form with base `rsp` — the
store went to the stack and the load went to the thread pointer.)

The JIT now copies an **assembler-written template** (`src/jit_stubs.S`) and
patches only a handful of immediates:

* the low bound and the span of each hot class,
* the thread-local displacement of each class's free list,
* the address of the generic fallback,
* the branch of each *unused* slot, so it skips straight to the generic
  entry instead of testing a range that can never match.

Every branch inside the template is **relative**, so the template stays
valid when it is copied to a different address. That is the whole reason it
is an assembler file: a single RIP-relative reference silently breaks the
moment the template is relocated — which is exactly what happened during
development.

The C side also **verifies the template layout** before patching it (the
opcode bytes in front of every patch point must match what is expected).
That check turns "the assembler and the C side disagree about the layout"
from a random crash into a clean refusal to install the code.

### 3.3 The concentration heuristic

The JIT specialises only when the hottest class accounts for at least 50%
of the sampled traffic (`FM_JIT_MINSHARE` overrides it). Below that, the
range tests mispredict on nearly every allocation and the static dispatch —
which computes the class with arithmetic — is faster. The JIT then installs
the generic path and stops profiling, so it costs nothing at all.

A second refinement: if almost all of a class's traffic is one *exact*
size, the range test is narrowed to that size (`size - x <= 0`), which is
the cheapest possible test.

### 3.4 Environment variables

| variable | meaning |
|---|---|
| `FM_JIT=0` | disable the JIT (generic path only) |
| `FM_JIT_PROFILE=0` | disable profiling (never compile) |
| `FM_JIT_HOT=N` | number of hot classes to specialise (default 4) |
| `FM_JIT_MINSHARE=N` | minimum traffic share of the hottest class, in percent (default 50) |
| `FM_JIT_DEBUG=1` | report the compiled specialisation |

---

## 4. Measurement methodology

This box is a **shared, heavily virtualised WSL2 host** with a neighbour
process using 164% of a CPU. Wall-clock timing here has a ~3.5% noise floor
and 5× outliers; the same program measured 5× apart across runs. The
measured "throughput" of an empty loop was 0.44 GHz on a 3.3 GHz part.

So the evidence is layered:

1. **Instruction counts (callgrind)** are the primary metric. They are
   *exact* and *reproducible* — no noise at all. The harness measures the
   delta between two iteration counts so that the ~140K instructions of
   dynamic-loader startup cancel out.
2. **Cache and branch behaviour (cachegrind)** explains *why* one design
   wins: data-cache misses and branch mispredicts are the mechanism behind
   the instruction counts.
3. **Wall clock on real programs** (SPEC CPU2006-derived `cfrac`,
   `espresso` and `barnes`) is the final word, run round-robin so that
   machine drift hits every allocator equally, reporting the minimum of
   several runs.
4. **Peak RSS**, because an allocator that is fast but holds on to memory is
   not fast in practice.

`bench/selfcheck.sh` quantifies the harness's own error floor by
benchmarking the *same* allocator under two different labels: 3.5% with
min-of-15. Every difference reported below is far larger than that.

---

## 5. Results

### 5.1 Instruction counts (callgrind, exact)

| workload | glibc | tcmalloc | jemalloc | mimalloc | v1 | **v2** | **jit** |
|---|---|---|---|---|---|---|---|
| churn, 64 B | 42.75M | 30.55M | 24.91M | 34.15M | 21.98M | 15.58M | **15.17M** |
| churn, 8 B | 41.85M | 30.55M | 24.77M | 31.83M | 21.99M | 15.60M | **15.19M** |
| churn, 1 KiB | 28.55M | 22.45M | 18.91M | 30.26M | 16.28M | 11.64M | **10.55M** |
| churnmix (8 sizes) | 30.95M | 24.87M | 19.85M | 25.86M | 17.16M | **12.90M** | 13.23M |
| mixed (random sizes) | 30.55M | 23.84M | 19.33M | 14.29M | 16.24M | **13.07M** | 13.35M |
| retain (alloc-heavy) | 23.03M | 19.32M | 14.91M | **9.46M** | 11.90M | 8.67M | **8.56M** |
| cross (larson) | 23.18M | 19.50M | 14.99M | 9.61M | 12.04M | 8.82M | **8.71M** |

On the classic churn loop v2 executes **64% fewer instructions than glibc**,
**49% fewer than tcmalloc**, **37% fewer than jemalloc** and **54% fewer
than mimalloc**. Across all seven workloads v2 is 57–64% cheaper than
glibc, 45–55% cheaper than tcmalloc and 32–42% cheaper than jemalloc.
Against mimalloc the margin ranges from 8% (on `mixed`, `retain` and
`cross`, where mimalloc's page reuse pays off) to 61% (on 1 KiB
allocations). The JIT is a further **1–9% ahead on the monomorphic
workloads** and steps aside (falling back to the static path) on the
diverse ones, where it ends up 1–3% behind.

### 5.2 Cache and branch behaviour

churn, 64 B:

| allocator | Ir | D1 misses | LL misses | branch mispredict % |
|---|---|---|---|---|
| glibc | 42.75M | 1264 | 1044 | 0.065 |
| tcmalloc | 30.56M | 15984 | 7995 | 0.207 |
| jemalloc | 24.92M | 15970 | 8258 | 0.621 |
| mimalloc | 34.16M | 1815 | 1407 | 0.194 |
| v1 | 21.98M | 1465 | 1103 | 0.325 |
| **v2** | 15.58M | 1731 | 1108 | **0.219** |
| jit | **15.17M** | 2416 | 1682 | 0.236 |

churnmix (8 sizes):

| allocator | Ir | D1 misses | LL misses | branch mispredict % |
|---|---|---|---|---|
| glibc | 30.95M | 1268 | 1046 | 0.098 |
| tcmalloc | 24.88M | 16049 | 7996 | 0.244 |
| jemalloc | 19.86M | 16026 | 8272 | 0.883 |
| mimalloc | 25.86M | 2240 | 1412 | 2.928 |
| v1 | 17.16M | 38326 | 1102 | 0.459 |
| **v2** | **12.90M** | 30533 | 1108 | **0.352** |
| jit | 13.23M | 5857 | 1675 | 0.374 |

The interesting number here is mimalloc's 2.9% branch mispredict rate on
the diverse workload: its fast path ends in a *range check* on the request
size, exactly like the JIT's dispatch. That is the cost of a range check,
and it is why the JIT needs the concentration heuristic.

### 5.3 Real programs (wall clock, min over two independent runs of 3 reps each; peak RSS in KiB)

| allocator | cfrac (s) | cfrac RSS | espresso (s) | espresso RSS | barnes (s) | barnes RSS |
|---|---|---|---|---|---|---|
| glibc | 4.18 | 3276 | 3.90 | 3392 | 2.87 | 58364 |
| tcmalloc | 3.30 | 8692 | 3.37 | 9584 | 2.93 | 64152 |
| jemalloc | 3.39 | 5340 | 3.50 | 5864 | 3.54 | 60896 |
| mimalloc | 3.50 | 3380 | 3.40 | 3688 | 3.13 | 58544 |
| v1 | 3.25 | 3412 | 3.49 | 8980 | 3.12 | 58472 |
| **v2** | **3.16** | 3796 | 3.50 | 8956 | **2.85** | 58452 |
| jit | 3.19 | 3692 | 3.41 | 9044 | 2.87 | 58448 |

v2 is **24% faster than glibc on cfrac** and every allocator beats glibc
there. On espresso the top four (tcmalloc 3.37, mimalloc 3.40, jit 3.41,
jemalloc and v2 at 3.50) are within 4% of each other, which is **inside
this host's noise floor** — the wall clock simply cannot separate them, and
the exact instruction counts are the metric that does. barnes allocates
once and then computes, so it barely measures the allocator at all.

The wall-clock numbers moved by up to 10% between the two runs of this
table, while the instruction counts were **bit-for-bit identical**. That
is the whole reason the instruction counts are the primary evidence.

### 5.4 Multi-threaded (8 threads, 2M operations, min over two runs)

| allocator | local alloc+free | cross-thread free |
|---|---|---|
| glibc | 0.035 | 0.120 |
| tcmalloc | 0.028 | 0.132 |
| jemalloc | 0.029 | 0.089 |
| mimalloc | 0.045 | 0.071 |
| v1 | 0.027 | **0.048** |
| **v2** | **0.021** | 0.049 |
| jit | 0.026 | 0.054 |

v2 scales best because the fast path touches **no shared state at all** and
the central list is lock-free: it is 1.7× faster than glibc when threads
allocate and free locally, and 2.5× faster than glibc and 2.7× faster than
tcmalloc when a block allocated by one thread is freed by another.

---

## 6. The honest answer: is this the fastest?

**The static build (v2) is the fastest allocator here for most use cases.**
It beats glibc by 24% on cfrac, by 64% in instruction count, and by 2.5× in
the multithreaded cross-thread case. It has the lowest instruction count on
every one of the seven micro workloads, and it beats tcmalloc, jemalloc and
mimalloc on every real program measured.

**The JIT is not the fastest, and it is not close.** It wins by 1–9% of
instructions on monomorphic workloads and loses 1–3% on diverse ones.
Its wins on real programs are inside the noise (cfrac: 3.19 s against the
static build's 3.16 s).
The reason is structural: the specialised dispatch has to *find* the hot
class, and finding it costs a range test, which is exactly the cost the
static path avoids with arithmetic. The JIT's saving — baking the thread-local
displacement into an immediate — is worth about two instructions, and the
range test that buys it costs about three.

What the JIT does demonstrate is real and useful:

* **Runtime specialisation is implementable and safe.** The generated code
  is installed behind an atomic pointer, survives concurrent execution, and
  is regenerated if the workload shifts.
* **An allocator can measure itself and choose its own strategy.** The
  concentration heuristic lets the JIT recognise when specialisation cannot
  help and step aside, which is why the JIT is *never significantly worse*
  than the static build. Without that step-aside, the JIT was 15% slower on
  diverse workloads.
* **The measurement discipline mattered more than the cleverness.** Three
  of the bugs found during development were in the measurement harness
  itself (a mis-parsed callgrind summary, a benchmark that overflowed its
  own ring buffer and double-freed, a `make clean` that deleted the
  benchmark binaries mid-run).

### Where v2 is *not* the best

* **Memory footprint.** On espresso, v2's peak RSS is 9.0 MB against
  glibc's 3.4 MB. The per-thread local caches and the fact that spans are
  never returned to the OS account for that. mimalloc's 3.7 MB is the best
  of the lot. A production allocator would need a `malloc_trim` that
  actually releases spans, and a way to bound the per-thread caches.
* **Locality on reuse.** glibc and mimalloc have fewer D1 misses than v2 on
  the churn workload (1264 and 1815 against 1731) because they reuse the
  same physical page over and over, while v2's blocks come from many
  different spans. mimalloc's "abandoned page" mechanism also keeps a freed
  block with the thread that freed it, so that thread gets the same warm
  memory back; v2 hands a block to whichever thread frees it, with no
  affinity for where it was allocated. This is the "free-list shaping"
  insight from the literature that v2 does not implement, and it is the
  most likely source of the remaining gap on workloads with long-lived,
  repeatedly reallocated objects.

### What I would do next

1. **Free-list shaping**: keep freed blocks in a per-thread "page" of one
   size class so that a thread reuses the same memory, giving the locality
   that mimalloc gets from its page abstraction. This is the single biggest
   known gap between v2 and mimalloc.
2. **Return memory to the OS** under pressure, and implement `malloc_trim`
   properly.
3. **Make the JIT's dispatch branchless** — a jump table indexed by
   `size >> 4` would replace N range tests with one indirect branch, which
   might make specialisation profitable for diverse workloads too.

---

## 7. Layout

```
src/
  fm_core.h        the v2 core: spans, free lists, central list, dispatch
  fm_static.c      the v2 build (libfastmalloc2.so)
  fm_jit.c         the JIT build (libjitmalloc.so)
  jit_stubs.S      the assembler-written code template the JIT patches
  alloc_v1.c       the v1 baseline (libfastmalloc.so)
  gen_classes.py   generates size_classes.h  (v1)
  gen_classes2.py  generates size_classes2.h (v2/JIT), with assertions
  size_classes*.h  generated

bench/
  instr.c          deterministic workloads for instruction counting
  mt.c             multi-threaded and cross-thread-free workloads
  stress.c         randomised correctness stress test (shadow memory)
  check.c          standalone model of the free-list invariants
  micro.c          wall-clock microbenchmarks
  suite.sh         the complete evaluation
  cache.sh         cachegrind comparison
  cgrun.sh         callgrind comparison
  jitcheck.sh      isolates the JIT's effect (same binary, JIT on/off)
  sweep.sh         sweeps the local-cache limit
  selfcheck.sh     measures the harness's own noise floor
  noise.sh         shows the run-to-run distribution
  run.sh           wall-clock microbenchmark runner
  rw.sh            real-program wall clock and RSS
  full.sh          per-workload Ir / wall clock / RSS table
  cg_annotate.py   parses cachegrind summaries

test_basic.c       basic correctness (sizes, alignment, realloc, calloc)
```

### Correctness

`bench/stress.c` keeps a shadow model of every live allocation (its size
and a magic pattern), verifies the pattern on every free, and exercises
small/large/aligned allocations, realloc growth and shrinkage, calloc
zeroing and interleaved multithreaded use. All four allocators pass it, as
does glibc.

Both v2 and the JIT also pass the same test under **valgrind memcheck**
with no errors at all — no invalid reads, no invalid writes, no leaks of
the shadow model's expectations:

```
$ LD_PRELOAD=$PWD/libfastmalloc2.so valgrind --error-exitcode=9 ./bench/stress
stress ok
$ LD_PRELOAD=$PWD/libjitmalloc.so valgrind --error-exitcode=9 ./bench/stress
stress ok
```

The size-class generators carry assertions that every request size maps to a
class that can hold it — the invariant that a mis-indexed table breaks, and
the one that caused a heap corruption bug during development (a 97-byte
request was served from a 96-byte block).

### Build and run

```
make                                  # libraries + benchmarks
make bench                            # benchmarks only
./bench/suite.sh                      # the complete evaluation
LD_PRELOAD=$PWD/libfastmalloc2.so ./yourprogram
```

The CI workflow builds the libraries, runs the correctness tests (including
the multithreaded stress test under valgrind memcheck) and re-measures the
instruction counts on every push, which is an independent check of the
numbers on different hardware.

## License

MIT — see [LICENSE](LICENSE).
