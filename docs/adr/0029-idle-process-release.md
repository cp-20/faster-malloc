# 0029 — Idle-process release: design pass, implementation deferred

*Status: Deferred (2026-11-13). Contemporaneous. This is the
design pass round 4's backlog asked for before any code; the
decision is to record the design and not build it yet.*

## The gap

ADR 0020's automatic release is driven by allocation activity:
a class's census runs from its flush path, gated by a
cost-proportional wall-clock interval. A process that frees a
large working set and then goes *completely* idle — no
allocations, no frees — never ticks a census again, so its dead
spans stay mapped until it wakes. The decay probe shows the
boundary precisely: with light continued activity the same
working set decays 135 → 9.5 MB in ~300 ms; with zero activity
it would sit at peak indefinitely.

Two honest bounds on the gap's size:

- Real daemons rarely allocate *nothing*: timers fire, logs
  write, health checks allocate. The bursty cell (round 5)
  models that trickle, and its idle phases do decay — the
  trickle drives censuses. The gap needs a literally silent
  process.
- glibc has the same shape: its heap trim runs on free/malloc
  activity (and only for the main arena's top). This is polish
  beyond parity, not a parity gap.

The harm case is real but narrow: a burst-then-silent daemon
whose resident peak is what an orchestrator accounts (cgroup
`memory.current` counts RSS), holding memory the machine could
use, for hours.

## Designs considered

**1. Dedicated timer thread (timerfd), armed on demand.**
A thread wakes on the census interval and runs censuses for
classes holding dead marks. Arming is lazy — created when the
first span is marked dead, exiting when no marks remain — so
short-lived and non-bursty processes never see it. The census
itself is already thread-safe (it operates on the central
stacks' atomic splices, the same slow path any worker thread
can take).

Costs, in ascending order of concern: idle wakeups in a process
that asked for none (bounded by the interval cap, and zero when
disarmed); one extra thread in every long-lived bursty process
(visible in `/proc`, in debuggers, in thread-count limits);
`fork()` — the thread does not survive fork, so a
`pthread_atfork` re-arm is mandatory, and a child that forks
*while the timer holds census state mid-splice* inherits a
consistent structure (the splices are atomic) but a stale
arming flag. All manageable; none free. A preload library that
spawns threads uninvited is also a deployment hazard in its own
right — it is the class of behaviour that gets allocators
blocklisted by cautious operators.

**2. Signal-based tick (setitimer + handler).** Rejected
outright. The census is not async-signal-safe: it walks and
re-links the very batches a flush on the interrupted thread may
be mid-splice on. Making it signal-safe would require deferring
to a flag the application must poll — i.e. activity again, which
is the status quo. It would also steal a timer and a signal
from the application.

**3. Piggyback on foreign activity** (hooks on `read`/`poll`
timeouts, etc.). No reliable, portable interception point
exists for a preload allocator; not seriously considered.

## Decision

**Defer.** The trigger conditions for revisiting, either of
which reopens this ADR:

1. A measured use case — a bursty variant with the trickle
   removed is the obvious construction — where a fully-idle fat
   process causes concrete harm, not a plausible one.
2. If built, it ships as design 1, opt-in first
   (`FM_IDLE_RELEASE=1`, outside the default profiles), with
   the atfork re-arm and arm/disarm lifecycle tested under the
   battery (fork-heavy and exec cases included), and graduates
   to a default only on the strength of its own measurements.

Until then, the documented behaviour stands: release is
activity-driven, and a process that wants its idle memory back
*now* has `malloc_trim` (ADR 0015), which works synchronously
and completely.
