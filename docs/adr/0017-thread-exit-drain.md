# 0017 — Drain per-thread caches when a thread exits

*Status: Accepted (2026-11-12). Recorded contemporaneously.*

## Context

Round 3's real-app matrix made retention the allocator's systematic
weakness: v2's peak RSS was the worst or near-worst of the seven
allocators tested in six of eight applications. One concrete hole
was already known but unmeasured: everything a thread holds in its
thread-local state — up to `kClassLimit` blocks per size class in the
local free lists, plus its entire large-block cache — was stranded
when the thread exited. The central lists are process-global, but
nothing ever moved a dead thread's local holdings back into them,
and cached large mappings were never unmapped. A process that
spawns short-lived worker threads (thread pools that resize, task
runtimes, servers shedding load) leaked allocator state on every
thread lifetime, bounded only by how much each thread happened to
touch before dying.

## Change

A `pthread_key` destructor now runs `fm_thread_exit_flush()` for any
thread that used the allocator's slow paths:

- Arming is lazy and off the fast path. `fm_arm_thread_exit()` is
  called only from `fm_refill_class`, `fm_flush_bin` and
  `fm_large_alloc2`; the hot small-allocation path never touches it.
- An `initial-exec` TLS state word (0 = unarmed, 1 = armed,
  2 = destructor running) makes arming a single TLS load on slow
  paths. The state is set to 1 *before* `pthread_once` /
  `pthread_setspecific` run, because pthread initialisation can
  itself allocate and re-enter; the destructor sets the state to 2
  before flushing, which makes the flush's own slow-path calls
  return immediately — `pthread_setspecific` is never called from
  inside the destructor, so the destructor cannot be rescheduled
  for another round.
- The flush returns every non-empty local bin to the central lists
  via `fm_flush_bin` and releases the thread's large-block cache via
  `fm_large_cache_drain` (the same code `malloc_trim` uses).

Side fix in the same change: the JIT build's hand-written `free`
never stored the FM_DEBUG free-block canary (`0xDEAD...` at offset 8)
that the static build's debug path checks, so an FM_DEBUG JIT build
asserted on ordinary frees. The canary store was added; both debug
builds now run the full battery.

## Measured effect

New regression test `bench/tlsexit.{c,sh}`: 24 threads × 3 rounds,
each worker allocates 4,000 small blocks and 8 × 128 KiB large
blocks, touches every page, frees **everything**, then exits. Final
process RSS after all threads are gone:

| build | final RSS before | final RSS after |
|---|---:|---:|
| v2 | 82,936 KiB | 2,852 KiB |
| JIT | 82,896 KiB | 2,820 KiB |

(glibc control: 1,892 KiB. The old build stranded ~81 MB of touched
pages purely as dead-thread state.) The test asserts the final RSS
stays within 32 MiB of the main-thread baseline for v2/JIT and is
wired into the standard battery.

No cost was measurable on the hot path: the arm check lives on
paths that already take locks or syscalls, and the macro suite is
unchanged within noise.
