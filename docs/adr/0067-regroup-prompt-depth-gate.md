# 0067 — Regroup prompt depth gate

Status: Rejected (2026-11-16). Implemented in `fm_release_maybe`, measured,
reverted.

## Attempt

The regroup prompt (fire a census when the stack head is uncharacterised, or
— since `ebc8593` — when the head batch is characterised but small while
depth ≥ 64) is what reorganizes a churn-fed stack between interval-driven
censuses. A huge uncharacterised stack (jq parking ~1.7 M blocks across
thousands of batches) makes the prompt fire a full census walk every 128
flushes. Gate the prompt by depth so huge stacks are priced by the adaptive
interval instead: `if (hd > LIMIT) return;` with LIMIT ∈ {64, 128, 512}.

## Measured

- LIMIT 64/128: locality/loc3 pages collapse to ~513 (the prompt is exactly
  what keeps a moderately deep churn stack regrouped before a burst; the
  stack accumulates regrouped chains + new batches beyond 128 between
  prompts).
- LIMIT 512: pages restored (67.8) and jq A/B looked dramatically faster in
  isolated runs (0.15–0.21 s / 0.52–0.57 s) — but an interleaved same-window
  comparison against HEAD showed those "HEAD" numbers had been taken under
  load (load average 4.2 from concurrent builds); re-measured at load ≈1.2,
  HEAD jq A was already 0.17–0.18 s and the gated build measured 0.20 s with
  server 27.2–28.6 ns vs HEAD 22.6–25.9 ns. No win, several regressions
  within noise or beyond it.

## Decision

The prompt's rate limiter (1-in-128 flushes) already bounds its cost, and the
interval mechanism prices big stacks. Revert; keep the prompt unconditional
on depth. Lesson recorded: **wall-clock comparisons must be interleaved in
the same load window** — this session twice produced phantom 1.5–3×
"improvements" that dissolved under interleaved re-measurement.
