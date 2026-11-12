# 0016 — Entry points must be callable before the library constructor runs

*Status: Accepted (2026-11-12). Recorded contemporaneously (round 3).*

## Context

Round 3 replaced lab benchmarks with real applications as the
primary evidence (bench/apps). The first real application tried
under the JIT build, Node.js, segfaulted at startup — even
`node --version` crashed. Python, jq and git happened to survive,
which is what makes this class of bug dangerous: it depends on
what the *host program's* initialisation order happens to be.

Root cause: an LD_PRELOADed library's constructor runs **after**
the constructors of the executable's own dependencies. The JIT
build's `malloc` jumps through the function pointer `g_entry`,
which was zero until the JIT library's constructor installed it.
Any dependency constructor that allocates before that point jumps
to NULL. libnode's C++ static initialisers allocate heavily from
their constructors, so Node hit it deterministically; the other
programs simply don't allocate in that window.

The static (v2) and v1 builds were unaffected: their entry points
are plain functions, callable from the moment the library is
mapped. Only the JIT's indirection was uninitialised state.

## Change

Commit 7d24e8d:

1. `g_entry` is initialised **at load time** (a static initialiser
   in the data segment, not constructor code) to `fm_jit_generic`.
   The generic path is safe that early: it touches only atomics
   and the main thread's TLS, both of which exist before any
   constructor runs.
2. The profiling entry point cannot trigger a JIT compile until
   the constructor has prepared the JIT state, so an early caller
   can never observe a half-initialised compiler.
3. Incidental fix found while reproducing: `FM_JIT_MINSHARE`
   parsing multiplied the configured percent by 100 a second
   time, so setting the variable at all made the concentration
   heuristic always decline to specialise (the JIT always stepped
   aside). The variable is a percent; it is now used as one.

## Evidence

Regression test `bench/earlyinit.sh`: builds a small program
whose dependency library allocates (malloc/calloc/realloc/free)
from its constructor and hands a built string to `main`, which
verifies it. Before the change the jit run segfaulted; after it,
all four allocators print `earlyinit ok`. The test is part of
the standard correctness battery.

Application-level confirmation: `node --version` and the Node
churn workload (bench/apps) run to completion under the JIT
build; before the fix both died during startup.

## Decision

**Accepted**, and it generalises into a standing rule for this
project: *every exported entry point must be fully callable
before the library's constructor runs, and a test must pin that
down.* Real programs initialise in orders the lab benchmarks
never produce; "works once main() starts" is not the contract a
malloc replacement is held to.
