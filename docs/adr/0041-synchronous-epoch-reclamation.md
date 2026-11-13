# 0041 — Synchronous epoch reclamation for central batch headers

Status: Rejected for adoption; disabled prototype retained (2026-11-13).

## Hypothesis

Replacing push/empty-pop mutex acquisition with short concurrent reader epochs
could reduce ADR 0038's central ownership cost. Keep the census mutex: header
lifetime alone cannot serialize a span's purge mask, withheld count and age.
Start from d9c43642d708bc50ff88e4892b49ab050f4701ff, not the unsafe round-6
control. Only this attempt is implemented; ADR numbers 0042–0043 remain unused.

## Change and ownership proof

`FM_RECLAIM_EPOCH` is an experimental **compile-time** switch, absent from the
normal Makefile and every runtime profile. The default uses the existing locks.
The prototype uses synchronous reader-count epochs (an admission-controlled
RCU-style grace period), not per-thread epoch numbers or an asynchronous retire
queue. One atomic word holds a closed bit and active-reader count. A reader
CAS-increments only an open word, and release-decrements on leaving. A reclaimer
serializes on the existing central mutex, atomically closes admission, waits
with acquire loads for zero readers, and reopens with a release store. Closing
admission prevents a continuous stream of new readers from starving the grace
period; it does not bound the time an existing reader takes to finish.

Push publishes a private header using acquire/release CAS under a reader epoch.
Pop reads the head/link and CAS-removes under a reader epoch. **A successful pop
leaves its epoch and completes a grace period before reading payload fields,
changing links, or returning the header to its TLS pool.** Unsuccessful pop
attempts retain their epoch across retries. A removed header remains immutable
until every possible stale reader has left. No header can be recycled and
republished while an old reader still has its address: this also prevents ABA.
`next` can remain non-atomic because all its writes occur while private, before
release publication or after a grace period. No plain link write overlaps an
old reader. Push counts its pending publication before CAS so a concurrent pop
cannot decrement zero; depth may overestimate while publication is pending.

A census takes the unchanged census mutex, flushes its local bin, closes the
stack epoch and drains all readers **before** exchanging the stack for NULL and
resetting depth. It reopens admission before walking, sorting, recycling,
madvise or munmap. Every detached header now belongs exclusively to the census;
new pushes land on the fresh stack, and already popped batches are absent.
Census failure republishes the same owned batches. No pointer from the detached
stack can be held by a stale pop. Two censuses still cannot race on a shared
span, even when they detach disjoint batches/classes. ADR 0035's TLS result
counters and atomic `dead_mark`, and ADR 0030's intersect/start-count and withheld
geometry, remain unchanged. The epoch is global, deliberately conservative.

Release publication/acquire observation protects initialized batch fields.
The closed-bit RMW and the release reader departures/acquire zero observation
order old link reads before reuse; reopening publishes the new epoch. A
successful pop does not hold a reader reference while requesting exclusivity,
so it cannot wait for itself. Lock order remains census then central; no
central-exclusive section requests census ownership. Reader sections perform
only atomics/link reads and sched_yield, with no allocation or callbacks.
The reader count assumes fewer than 2^31 simultaneous allocator readers.

## Thread exit, stalls, and fork

- **Thread exit:** no registration slots, TLS hazard records or retired queues
  need a destructor. Each operation leaves its epoch before returning; ordinary
  pthread exit/deferred cancellation occurs outside these short sections.
  `sched_yield` is not a cancellation point. The existing TLS destructor still
  flushes caches through the same publication protocol. Asynchronous cancellation
  or abrupt thread termination inside malloc remains unsupported, as with the
  baseline mutex implementation; never clear another thread's reference by time.
- **Stalled reader:** a suspended reader pins the entire global grace period.
  Reclaimers wait and admission stays closed; no timeout declares it dead.
  Memory safety wins over progress. There is no unbounded retired-header queue,
  but other central operations, trim and fork can stall and the yielding wait
  consumes scheduler/CPU budget. This is not a lock-free allocator or a bounded
  latency design. Sharded/asynchronous epochs would be a separate attempt.
- **Fork:** prepare takes census ownership then central exclusivity, closes
  admission and drains readers before the snapshot. Parent and child reopen
  the empty-reader epoch and release the same locks in reverse order. No reader
  count from a vanished thread survives. A popped batch privately held by a
  vanished thread can remain unreachable in the child, just as vanished TLS
  caches could in the baseline; it is never guessed free or republished. Child
  timer disabling and exec-only restart rules are unchanged. Fork in a signal
  handler interrupting an allocator operation is unsupported (baseline also
  cannot recursively acquire its mutex). Atfork registration failure retains
  the baseline limitation: no multithreaded allocator-fork safety claim.

## Measured numbers and decision

Same compiler/O3 v2, exact d9c4364 git-object control, alternating order,
one warmup pair then five measured pairs per case. Wall time includes process
startup/exit; no fastest-sample selection. The host is a shared cloud VM:
Intel Xeon Platinum 8573C, three visible CPUs, cgroup quota two CPUs
(`cpu.max=200000 100000`), GCC version in the host artifact. This is not a
quiet reference-machine refresh. No unrelated test battery ran during samples.

| case | locked median (range), s | epoch median (range), s | delta |
|---|---|---|---|
| local | 0.401493 (0.392481–0.407754) | 0.529337 (0.506599–0.548309) | +31.84% |
| cross | 0.351389 (0.311064–0.408383) | 0.302966 (0.281031–0.481931) | -13.78% |
| many | 3.064352 (2.153387–3.722924) | 4.310923 (3.088391–4.685942) | +40.68% |
| census | 3.923803 (3.122595–4.796691) | 3.835392 (3.228878–4.667090) | -2.25% |
| bursty | 7.611019 (7.550039–7.708372) | 7.555330 (7.547095–7.734992) | -0.73% |

`bench/ownership.c` is unchanged: four producer/consumer pairs, 128 requests
mapping to 64 v2 classes, 200,000 transfers per pair (800,000 total), endpoint
bytes and final counts asserted. Every final sample passed. Local/cross use
10,000,000 operations; cross uses two threads despite the final `8` argument.
Bursty uses the existing app-matrix source, unchanged operation budgets and
sleeps. Every final checksum is `230115700662`. Bursty idle2 RSS, KiB, median
(range): locked 43052 (42068–43484); epoch 43060 (42316–43344).
Bursty is the relevant reclamation/daemon cell; no broader app-matrix
promotion claim is made. Its wall time is sleep-dominated, so do not infer application throughput from
the small time difference or memory superiority from the overlapping RSS.

**Reject adoption.** Local churn regresses with separated ranges; many-class
traffic has a materially slower median. Cross-free and census medians improve
but have wide overlapping ranges; neither establishes a verified win. The
preliminary series also changed direction for cross-free and census. The
synchronous global counter/grace-period cost has not justified replacing the
locked implementation. No claim isolates mutex-only cost: code layout, epoch
atomics, waiting and scheduling all contribute. Keep the prototype and tests
for review/reproduction, disabled in normal builds; do not change defaults.

### Retained preliminary evidence

The first control used semantically equivalent helper functions around lock
calls, changing compiled `.text`. It is not the final exact-code control.
It completed five measured pairs for each ownership case, then its first
bursty warmup died with SIGSEGV (-11). The initial runner captured no failed
stdout in the artifact; only its traceback remains. Subsequent standalone
original/prototype bursty runs passed. The failure is unexplained, not evidence
of a reclamation win and not attributed to untouched main. Macro aliases now
preserve the default code, whose `.text` exactly matches the original build.
All final samples were rerun with the original git-object control. Do not
silently pool or replace preliminary samples:

| case | helper-control median (range), s | epoch median (range), s |
|---|---|---|
| local | 0.420100 (0.400739–0.514444) | 0.544791 (0.501424–0.703130) |
| cross | 0.326558 (0.280027–0.384650) | 0.459493 (0.350633–0.623868) |
| many | 3.600761 (1.822631–4.489711) | 3.789745 (3.037773–4.900488) |
| census | 2.907462 (2.384588–3.055299) | 3.486652 (3.139424–3.877680) |

Raw samples, warmups, phase outputs, discarded-control traceback, host metadata,
summary and correctness transcripts are under
`bench/results/round9-reclamation/`. Reproduce with
`bash bench/round9/measure.sh` and `bash bench/round9/verify.sh`.

## Validation

Full locked and epoch `make`/round7 batteries pass, including both v2/JIT and
the idlelifecycle custom-build fault matrix. The stalled-reader regression and
all twelve FM_DEBUG stress runs pass (three per implementation/configuration,
with zero release age, minimum one purge page, idle release and realloc-cache
search enabled). Normal builds were restored. Details and transcripts are in
the round document and results directory.

The first locked battery hit SIGBUS in JIT footprint `test_basic`, with normal
machine code matching untouched main. Fifteen original-JIT profile checks and
the subsequent full batteries passed. The failure is retained under `failures/`
and remains unexplained; it is not claimed fixed by rerunning.
These tests and the ownership argument are not an exhaustive race/sanitizer
proof or evidence of bounded latency with a stalled reader.
