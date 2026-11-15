# 0070 — Rejected micro-probes and diagnostics (2026-11-14 … 2026-11-16)

Status: All rejected or diagnostic-only; recorded so the probes are not
retried without new evidence. Each was implemented, measured, and reverted
unless noted as diagnostic.

## Instruction/memory micro-probes

- **Prefetch in the five free-list walk sites** (flush count walk, census
  count/coverage, rebuild): D1 unchanged (locality 2,224,871; loc3
  1,127,323), Ir churn64 58.6 / churnmix 69.7 (+1.0/+0.8). Reverted; the
  *census-slot* prefetch variant was later adopted (51936cf) after narrowing
  the prefetch to the slot index computable from the link word.
- **`fm_malloc` prefetch hint locality 1 → 3**: no D1 change (2,224,874),
  locality 75.0 ns. Reverted.
- **Auto-release tick as flushed class + 16-class round-robin** instead of
  the full 145-class sweep: Ir slightly better (57.2/68.4/89.0/64.3/63.5,
  D1 2,213,388) but coverage of idle classes per tick drops and the win is
  below the noise floor of every wall-clock metric. Reverted.

## Release/purge configuration probes (code defaults)

- **`page_purge` default 0**: Ir retain 62.3 / cross 63.2 (better), mt
  0.12 s/5636 KB, but trim after_free unchanged and partial-span RSS cases
  lose their purge path. Not adopted.
- **`purge_min_pages` 4 → 1**: no measurable change (mt 0.14 s/6260 KB).
  Reverted.
- **Regroup prompt trigger ×2 (every 64 flushes)**: mt 0.18 s/9380 KB,
  locality 76.9 ns, D1 2,405,448. Reverted.
- **`FM_RELEASE_AGE_MS=10 FM_RELEASE_KEEP=1` / `=0,KEEP=0` (env probes, no
  code change)**: large mt RSS/trim improvements at real cost to locality
  ns and macros; unresolved tradeoff, left as runtime tuning knobs.

## Diagnostics (no code kept)

- **`fm_trim_spans` caller-TLS flush + bump-remainder drain before trim**:
  after_free/after_trim unchanged (161464/7104 vs 161480/7060). malloc_trim
  already flushes per class; the bump remainder is not what blocks unmapping.
- **Census instrumentation** (which classes fire, batch composition): 12
  censuses per locality-10 run (not the dozens assumed); jq A fires 33 across
  7 classes. This killed several plausible-sounding "reduce census
  frequency" ideas and redirected work to per-census cost.
- **Block accounting instrumentation for ADR 0069** (chained/res vs
  walked/pure, per-entry chain-length checks): produced the unresolved
  evidence recorded there.
- **Batch-header pool instrumentation** (`fm_batch_alloc` page mmaps):
  exposed the 126-headers-per-page orphan leak fixed in `ebc8593`.

## Corruption-classified attempts (never adopted)

- **Flush count walk removal via `kClassLimit − fm_cnt`**: `fm_cnt` is a
  countdown, not a list length; published counts diverge from chains →
  CORRUPTION (free) + segfault. Reverted immediately.
- **Residue/unknown republish variants that reuse or skip batch-header
  lifecycles** (two of the ADR 0063 variants): header lifetime rules require
  every published batch to own a header until consumed; short-circuiting
  that corrupts. The rule stands: a batch must have exactly one owner at a
  time, allocated fresh or still owned by the census's detached stack.
