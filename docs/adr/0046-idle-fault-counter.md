# 0046 — Make the idle creation fault counter race-free

Status: Accepted (2026-11-13).

## Hypothesis

The TSan lifecycle retry build reports a read/write race on the function-local
`failures` counter in `fm_idle_kick`. The timer-state compare/exchange prevents
simultaneous creation attempts, but resetting that state with a relaxed store
does not publish the non-atomic counter to the next creator. The injected
first-failure path is therefore not race-free in the C memory model.

## Change

Make the compile-time fault counter atomic and use relaxed loads/decrements.
This code exists only under `FM_TEST_IDLE_CREATE_FAILURES`; production timer
behavior and synchronization are unchanged. The existing concurrent idlefork
retry scenario is the regression and remains in the full lifecycle matrix.

## Measured numbers

Before: namespaced TSan JIT idlefork-retry exits 66 with one reported race on
`failures` (read at fm_core.h:1563, write at 1564). After: all 144 lifecycle build/run steps pass (72 runtime cases), including
both retry variants under TSan and TSan+UBSan. Exact before/after diagnostics
are retained with the round-9 “Sanitizer proof” section. No performance conclusion is drawn from these runs.

## Adopt/reject with reason

Adopt. It fixes a confirmed race in the injected-failure test path without
changing allocator behavior in production builds or weakening the test.
