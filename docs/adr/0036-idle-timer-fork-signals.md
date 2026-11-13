# 0036 — Idle timer: fork quiescence and signal ownership

*Status: Accepted hardening of the opt-in prototype (2026-11-13).
Default promotion rejected pending downstream measurements and lifecycle
work. Supersedes ADR 0032's child respawn policy.*

## Audit findings

ADR 0032 registered an atfork child handler every time a timer creation was
attempted, ignored registration failure, and reset its spawn guard in the
child. The next flush could then call pthread_create in a multithreaded
fork child, where POSIX permits only async-signal-safe calls before exec.
The timer could also be mid-census at the fork snapshot, leaving detached
central supply and census state owned by a vanished thread. Calling the
census "lock-free" did not provide snapshot quiescence.

The timer inherited its creator's signal mask: application signal handlers
could unexpectedly run on this new thread. Finally, writing its old sweep
time over the shared last-activity timestamp after a sweep could overwrite
a newer concurrent flush and make the next idle decision premature.

## Decision

- Register handlers once with the existing thread-exit pthread_once, before
  a timer can be spawned. If registration fails, never start a timer.
  Creation failures leave the guard retryable without registering more handlers.
- Ordinary pthread-aware fork prepares by acquiring census ownership and then
  central ownership (ADR 0035), waiting for an in-flight sweep to publish all
  its supply. Both parent and child release the locks held by the forking
  thread. The handlers allocate nothing and create no thread.
- The child marks the timer disabled, not merely unstarted. No child timer
  can leak, respawn or fire even after child allocation activity. Exec loads
  fresh allocator state and can opt in again. This is an intentional change
  to ADR 0032; the parent keeps exactly its original timer.
- Block application signals around creation so the worker inherits a blocked
  mask; restore the creator's exact mask on every path. Create detached from
  birth with pthread attributes rather than creating joinable then detaching.
  Handle attribute/mask/create errors without publishing a running timer.
- Keep sweep cadence in a worker-local timestamp. Only activity writes the
  last-activity atomic, so sweep completion cannot erase newer activity.

## Signal-safety boundaries

No signal handler or application timer is installed. The worker uses
nanosleep and monotonic elapsed time, so interruptions cannot by themselves
make a one-second idle threshold expire early. Census and allocator entry
points remain **not async-signal-safe**. Fork from a handler that interrupted
allocator ownership can deadlock and is unsupported; `_Fork`, vfork and raw
fork syscalls do not acquire these handlers. glibc's internal cancellation
signals remain under libc's control. Explicitly targeting a worker with a
signal is outside the process-directed-signal guarantee.

Normal Linux fork children can exercise the existing allocator paths, but
this is not a claim of POSIX permission to call malloc after multithreaded
fork. Inherited caches of other vanished application threads are a separate,
pre-existing allocator limitation. Dynamic unload while the detached worker
runs remains unsupported; no dlclose lifecycle mechanism is added this round.

## Validation and measurements

`bench/idlefork.c`: eight simultaneous burst starters exercise spawn-once;
the creator's entire signal mask is compared before/after; 30 SIGUSR1
notifications must execute on the application caller rather than the timer.
Four ordinary forks run while four workers churn: one child stays silent,
three churn, all remain single-threaded through a 2.2-second idle window.
The parent still has exactly one extra thread after workers join. Repeat
with the flag off, under v2 and JIT. A 40-second alarm bounds deadlocks.
No timer thread in the child means there is no child sweep source.

On the hardened v2 idleprobe, flag off: drain 39,940 → idle-6s 39,944 KiB;
flag on: 40,028 → 28,340 KiB, first drop at 1.5 seconds, settled by 2.5.
This preserves the parent's silent-release behavior. These are **indicative
cloud-container RSS measurements**, not comparable to the reference VM.
The timer remains default-off in every profile. See the round-7 log for
all correctness results and downstream re-measurement requirements.
