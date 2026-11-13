# 0045 — Remove data races in JIT profiling

Status: Accepted (2026-11-13).

## Hypothesis

TSan reports concurrent reads/writes of `g_profile_events` in `fm_sample`.
A non-halting eight-thread reproduction also reports `g_give_up` written in
`jit_compile` while another thread reads it in `fm_sample`. Approximate
profiling does not make unsynchronized C accesses defined. The compiler may
exploit these races even when lost samples would otherwise be harmless.

## Change

Return the incremented value of the existing relaxed atomic `g_samples`
from `fm_profile` and use it for the 256-sample compile trigger. Delete the
redundant non-atomic counter. Make the give-up flag atomic, with relaxed
loads/stores: it is a hint, while entry publication already uses release.
This adds no extra atomic increment to the sampled path.

Add `bench/jitprofile.c`: eight threads perform 100,000 allocations each via
volatile function pointers. `FM_JIT_MINSHARE=101` forces the broad-distribution
fallback, exercising the trigger and the give-up flag. Wire it into make,
clean, the normal correctness battery, and the instrumented suites.

## Measured numbers

Before: the eight-thread TSan reproduction reports 3 warnings, comprising
read/write and write/write conflicts on the trigger counter and a write/read
conflict on the give-up flag. The existing idlefork create/retry scenarios
also exit 66 on the trigger race. After: the fully instrumented eight-thread regression completes all 800,000
allocations with exit 0 and no warning under both TSan and TSan+UBSan; the
broad-distribution fallback is logged in both. Full matrix results are in
the round-9 “Sanitizer proof” section and its retained case results.
No performance claim is made from sanitizer timings.

## Adopt/reject with reason

Adopt. Both reports correspond to actual concurrent unsynchronized C accesses,
not sanitizer artifacts. Reusing the existing sample count preserves the
intended aggregate trigger interval without adding an atomic operation;
atomic flag access removes undefined behavior without changing allocation
policy or disabling profiling.
