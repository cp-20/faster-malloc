# 0050 — Zero-age span release for short-lived apps

Status: Rejected (2026-11-13).

## Hypothesis and change

Perhaps a process peaks before the second census can age spans. Test footprint
with `FM_RELEASE_AGE_MS=0`, `FM_RELEASE_INTERVAL_MS=0`, and
`FM_RELEASE_MAX_INTERVAL_MS=0` (existing parser clamps remain in force). This
isolates an aggressive existing-knob policy before adding pressure-triggered
release logic. No production change for this attempt.

## Measurements and decision

Five interleaved same-session samples appear as `eager` in
`bench/results/round10-shortlived/matrix.jsonl`; the round-10 short-lived section
reports medians and ranges. Zero age does not produce a material additional
peak reduction over footprint; large-cache removal accounts for the useful
change. Reject the eager policy and avoid adding pressure/ageing machinery.
This rejection concerns peak RSS of these apps, not zero-trickle release latency.

Five runs each, median (min–max), same-session measurements:

| App/configuration | Peak RSS KiB | Seconds |
|---|---:|---:|
| python/footprint | 448,828 (448,744–448,828) | 2.847 (2.784–3.026) |
| python/eager | 448,828 (448,820–448,832) | 2.791 (2.695–3.016) |
| node/footprint | 450,492 (449,912–450,812) | 1.289 (1.215–1.349) |
| node/eager | 449,772 (449,124–450,632) | 1.321 (1.260–1.602) |
