# 0048 — Existing footprint profile for short-lived apps

Status: Rejected as the recommended short-lived preset (2026-11-13).

## Hypothesis and change

Existing `FM_PROFILE=footprint` should reduce Python/Node peak through its 8 MiB
large-cache cap and earlier span release. No production change for this attempt.
Compare five runs with balanced, glibc, jemalloc and mimalloc in the same session.

## Measurements and decision

The short-lived section of `docs/round10-2026-11-13.md` contains all medians and
ranges; raw samples are in `bench/results/round10-shortlived/matrix.jsonl`.
Footprint improves peak versus balanced, but retaining no large blocks saves
more, without changing small-span release policy. Keep footprint available for
its existing use cases; do not promote it as the short-lived recommendation.

Five runs each, median (min–max), same-session measurements:

| App/configuration | Peak RSS KiB | Seconds |
|---|---:|---:|
| python/v2 | 502,884 (502,884–502,884) | 2.927 (2.674–3.000) |
| python/footprint | 448,828 (448,744–448,828) | 2.847 (2.784–3.026) |
| node/v2 | 498,892 (497,072–500,372) | 1.342 (1.277–1.564) |
| node/footprint | 450,492 (449,912–450,812) | 1.289 (1.215–1.349) |
