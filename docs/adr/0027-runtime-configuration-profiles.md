# 0027 — Runtime configuration: FM_PROFILE and per-knob FM_* overrides

*Status: Accepted (2026-11-13). Contemporaneous. Shipped in the
same commit as ADR 0024 (`4c192bb`); this entry was written the
same night, from the code and the sweep data, to give the
configuration surface its own record.*

## Context

By the end of round 4 the allocator had accumulated a set of
slow-path tuning constants with real, measured trade-offs behind
them: dead-span age, per-class retention quota, census depth
gate and interval policy (ADR 0020, revised after the jq
incident), the large-block cache budget (ADR 0019), and the
regrouping fragment threshold (ADR 0024). Compile-time constants
force one weighting of speed against footprint on every
deployment, and rebuilding to retune is not something a library
loaded by `LD_PRELOAD` into arbitrary programs can ask for.

At the same time, the hot path's discipline is absolute: no
environment reads, no parsing, no branches on configuration in
`malloc`/`free`. And ADR 0016's lesson stands — entry points
must work before any constructor runs, so configuration cannot
be read in a constructor either.

## Design

`fm_config_init()` in `src/fm_core.h` builds one `FM_Config`
struct and publishes it as `fm_cfg`:

- **Parsed once, lazily**, via `pthread_once`, on the first
  *slow* path (refill, flush, census, large alloc/free) —
  reached through the `FM_CFG()` macro. Hot paths never touch
  it; pre-constructor callers never trigger it; by the time any
  knob can matter, the environment is stable.
- **`FM_PROFILE`** selects a named preset for the whole knob
  set: `balanced` (the defaults — exactly the constants the
  previous rounds shipped), `throughput`, or `footprint`.
  Unrecognised values fall back to `balanced`.
- **Individual `FM_*` variables override the profile**, one per
  knob: `FM_RELEASE_AGE_MS`, `FM_RELEASE_KEEP`,
  `FM_RELEASE_MIN_DEPTH`, `FM_RELEASE_INTERVAL_MS`,
  `FM_RELEASE_MAX_INTERVAL_MS`, `FM_RELEASE_WALK_DIV`,
  `FM_REGROUP_MIN`, `FM_LCACHE_MAX_BYTES`. Malformed values are
  ignored; numeric values are clamped to sane ranges (age
  0–600 000 ms, cache budget 0–16 GiB, …). A bad environment can
  make the allocator slower or fatter, never incorrect.

The presets, with their evidence:

| knob | balanced (default) | throughput | footprint |
|---|---|---|---|
| release age | 100 ms | 1000 ms | 25 ms |
| release keep / class | 2 | 8 | 0 |
| census min depth | 4 | 16 | 2 |
| census interval floor | 10 ms | 50 ms | 5 ms |
| census interval cap | 5 s | 30 s | 1 s |
| walk divisor | 100 | 25 | 400 |
| large-cache budget | 64 MiB | 256 MiB | 8 MiB |

ADR 0026's sweep is the evidence base: census frequency trades
~7 % deep-stack time for ~4 % RSS at 4× (div 100 → 400), age
moves release *latency* almost 1:1 and nothing else, and keep
is a pure retention premium. `throughput` sits on the calm side
of every one of those trades and quadruples the large-cache
budget (ADR 0019's knee was measured for the general case, not
for large-block churn); `footprint` sits on the eager side and
cuts the cache budget 8×. `balanced` changes nothing for
existing users — it *is* the previous build's constants.

## Alternatives considered

- *Constructor-time init*: rejected by ADR 0016 (Node.js called
  entry points before the constructor; the same window would
  read a half-initialised config).
- *A config file or a programmatic API*: a preload library has
  no reliable file context and no caller to register with; the
  environment is the one channel every loader path honours.
- *Per-knob variables only, no profiles*: the knobs interact
  (age × interval × keep jointly set the release behaviour);
  asking users to compose them from raw constants is how
  mis-tuned deployments happen. Profiles are the interface;
  the knobs are the escape hatch.

## Consequences and limits

- The JIT build shares `fm_core.h` and therefore the same
  configuration surface; its fast path is as untouched as v2's.
- The knobs govern the *release and cache* machinery only.
  Size classes, local-cache limits and the central batch design
  remain compile-time: they shape the data structures
  themselves, and the audit (ADR 0028) found no deployment-
  specific reason to move them.
- Profile validation is empirical, per round: the round-5 doc
  measures all three profiles across the app matrix and the
  footprint probes, and records where each profile's trade
  actually lands. A profile that stops earning its name gets
  its constants revised — in the open, in a later ADR.
