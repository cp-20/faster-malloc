# 0032 — Zero-trickle retention, measured; FM_IDLE_RELEASE prototype

*Status: Accepted (2026-11-13). Contemporaneous. Round 6, item 3.
Implements ADR 0029's design 1 behind its reopening trigger.*

## The question

Rounds 4–5's decay results all relied on a trickle: light
continued activity drives the censuses that age and release.
ADR 0029 deferred an idle-process release mechanism pending one
measurement — a bursty variant with **no** activity after the
burst. If a fully silent process retains badly, prototype the
timer-thread design; if the field shares the behaviour and the
level is set at drain time, record that instead.

## Measurement

`bench/apps/idleprobe.c`: ramp to ~92 MB live (bursty's
random-slot shape), drain to ~12 MB live, then six seconds of
total silence, RSS sampled every 500 ms. Output is `write()` on
a stack buffer; the idle window performs no allocation.

| allocator | drain RSS | idle 6 s RSS |
|---|---|---|
| glibc | 36.6 MB | 36.7 MB |
| v2 (round 5 build) | 40.8 MB | 40.9 MB |
| v2 (round 6, purge) | 40.8 MB | 40.8 MB |
| mimalloc | 43.5 MB | 43.5 MB |

Every allocator in the field is **flat for the entire silence**.
All release machinery here is activity-driven — mimalloc's
included — so a silent process keeps exactly what its drain
dynamics left. v2's silent level sits mid-field: above glibc
(whose drain-time trim is more aggressive during the free burst
itself) and below mimalloc. Retention "bites" only in the sense
ADR 0029 framed: a burst-then-silent daemon holds its drain-end
RSS indefinitely, and no incumbent fixes that.

## Decision: prototype, opt-in

`FM_IDLE_RELEASE=1` (default **off**, no profile sets it)
implements ADR 0029's design 1:

- A detached timer thread is spawned lazily on the first flush
  tick (CAS-guarded, never from inside a census).
- Each tick stores its wall time in an atomic. The thread wakes
  every 200 ms; after a full second with no tick it runs the
  ordinary ageing censuses (`fm_trim_class` with the configured
  keep quota) for every class whose central depth clears the
  usual gate, then re-arms for another second. Each sweep is a
  full ageing step, so marks land, age, and release across
  successive sweeps.
- Fork: an `pthread_atfork` child handler resets the state —
  the child has no timer thread (only the forking thread
  survives), so its next tick respawns one. The thread holds no
  locks; the census is lock-free by design (atomic detach), so
  there is no lock state to repair in the child. Forking while
  another thread is mid-census carries the same best-effort
  caveat as forking mid-`malloc_trim` anywhere.

Measured on idleprobe with the flag on: RSS falls 41.1 → 29.5 MB
during the silence (first sweep's marks at ~1.5 s, release by
~2.5 s) — the only allocator in the field that returns anything
while silent. It does not reach the 12.1 MB live floor: the
remainder is partially-free spans' pinned pages and ordinary
retention, which no mechanism in the field returns either.

## Why default-off stands

A thread in every process is a real cost: it changes the
process's thread census for embedders, interacts with
fork/exec-heavy short-lived tools for zero benefit (they never
idle for a second), and ADR 0029's harm case is specifically
long-lived daemons — whose operators can opt in. The battery
(test_basic, stress, check, mt both modes, a fork+churn+idle
test) is green with the flag on and off; the flag is v2/JIT
shared via fm_core.h.
