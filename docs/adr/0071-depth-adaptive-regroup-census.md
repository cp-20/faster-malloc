# 0071 — Depth-adaptive regroup-only census for shallow central stacks

Status: Accepted (2026-11-15, commit 667cf2e).

## Change

`fm_trim_class` gains mode 2 (regroup-only: detach, count, span-group,
rebuild — no dead-mark, page-purge coverage/decision/apply, reclaim or
unmap). `fm_release_maybe` runs it when the central stack depth is at or
below a new `FM_REGROUP_ONLY_DEPTH` threshold (default 32, env-overridable),
and the full release census above it.

## Rationale (measured)

The span-grouped rebuild is what concentrates recycled blocks (locality
pages 67.8 vs 512.9 with release disabled) but the full census's
mark/purge/coverage machinery costs ~2× locality ns/op and ~0.7M extra D1
misses. Shallow stacks regroup cheaply; deep stacks keep the release
behavior that returns memory.

Note: as implemented, mode 2 reuses the `instant` code path (immediate kill
with the keep quota, no ageing) — the regroup/kill behavior is what drives
the measured wins; the "regroup-only" name in the commit message is
aspirational.

## Results

mt 1/2/4/8M RSS 2748/2924/2832/3100 KB (previously ~3972–9160), trim
after_free/after_trim 152968/6768 KB (previously ~161420/7050), Ir/op
unchanged, locality pages preserved (67.8), v2/jit test_basic+stress pass,
loc3 67/68 pages.
