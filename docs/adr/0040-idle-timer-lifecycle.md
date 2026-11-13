# 0040 — Idle timer lifetime: refuse unmapping with ELF NODELETE

*Status: Accepted (2026-11-13). Extends ADR 0036. Timer remains default-off.*

## Context

A detached timer executes allocator code indefinitely. Merely stopping it in
an unload destructor does not establish an allocator unload protocol: other
threads can still execute allocator calls, hold allocations, or have pending
pthread-key callbacks into this DSO. Joining a worker under the dynamic loader
lock can also deadlock with loader work on that worker. An atfork callback
cannot safely create a replacement thread in a multithreaded fork child.

## Decision

The shipped v2 and JIT DSOs link with `-Wl,-z,nodelete`. The ELF `DF_1_NODELETE`
flag instructs the Linux/glibc loader to retain their code, data and TLS even
when the final explicit `dlopen` handle is closed. **Physical unload is refused
by the loader**; `dlclose` itself returns success, not an error. This applies
with the timer off too, because application-thread callbacks and outstanding
allocations have the same lifetime problem. There is no new shutdown API.
Reopening the library reuses its state and at most one detached timer, rather
than registering fresh keys/handlers or accumulating workers. This is not a
claim that a library with outstanding allocator users can be physically
unmapped safely. Custom v2/JIT builds must preserve the NODELETE link flag;
builds or loaders that omit/do not honor it are outside this contract. V1 has
no idle timer and is unchanged.

After `dlclose`, an enabled worker continues ordinary idle maintenance. It
keeps application signals blocked and needs no dynamic-loader call. No exit
or unload destructor cancels, joins, or locks against it. On normal `exit` or
return from main it may run concurrently with exit handlers, just as it does
with application allocator calls. The OS terminates it and reclaims its stack,
TLS, mappings and allocator state at process termination. `_exit`, `_Exit`,
exec and fatal termination likewise rely on OS reclamation; no final sweep,
cache drain, RSS reduction, or persistent cleanup is promised. `pthread_exit`
of the last application thread is **not process shutdown** while this worker
is alive: applications using the opt-in timer must use exit/return-from-main
for process termination. NODELETE also retains static TLS capacity and cached
allocator mappings until termination; this bounded resident lifetime is an
intentional cost, not a per-dlclose resource leak.

## Startup failures

Atfork registration still happens once, before any timer creation. Failure
leaves `fm_atfork_ready` false forever: no worker is started. No registration
is retried and no duplicate handlers accumulate. Without registered handlers,
multithreaded fork cannot rely on allocator lock quiescence; use exec/spawn
without allocator activity in the child. Timer failure handling does not
repair that broader fork limitation.

A pthread_create failure leaves no worker and resets the spawn guard for a
later allocator flush to retry. Attributes are destroyed and the creator's
signal mask restored on failure as on success. The application allocator
continues working. Compile-time `FM_TEST_IDLE_ATFORK_FAIL` and
`FM_TEST_IDLE_CREATE_FAILURES` inject only timer-path failures. The latter
uses -1 for permanent failure or a positive count for transient failures;
the spawn guard serializes access to that test counter. Production builds
contain neither fault control nor an environment-driven failure switch.

## Fork child re-enablement

Keep the child disabled until exec, including after later allocator activity.
There is no justified API point in this LD_PRELOAD allocator where it can
infer that the application has left the POSIX async-signal-safe-only window.
A new restart API would add a permanent public lifecycle contract without a
caller/use case, and could encourage invalid post-fork pthread_create calls.
Exec is the explicit safe lifecycle point: a fresh image reads configuration
and may start a fresh worker. No hidden restart or public restart API is added.
All ADR 0036 signal and fork boundaries continue to apply.

## Validation

`bench/idlelifecycle.sh`, included in `bench/round7.sh`, checks ELF NODELETE
on both shipped DSOs. With the timer off/on, a fresh process late-loads and
closes the library twelve times, verifies it remains discoverable with
RTLD_NOLOAD, exits application threads with allocator-key callbacks after
closing handles, and checks the task count never accumulates workers. It
waits three seconds after close to exercise worker code, then returns from
main with an active worker. A process alarm bounds hangs. The harness reserves
glibc optional static TLS at startup because initial-exec TLS already limits
late loading; this change does not promise arbitrary late-dlopen support.

For both implementations the same script builds permanent atfork failure,
permanent creation failure, and one-shot creation failure variants. Closed
handle tests require zero workers for permanent failures and exactly one for
retry recovery. Creation-failure variants also run the existing concurrent
spawn/signal/fork regression. Failure creates no thread resource to leak;
attribute cleanup and mask restoration remain on their shared cleanup path.
The permanent atfork failure deliberately does not test multithreaded fork
without quiescence handlers. Established correctness and FM_DEBUG results
are recorded in the round-8 Timer lifecycle section.
