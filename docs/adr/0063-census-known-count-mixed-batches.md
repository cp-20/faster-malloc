# 0063 — Census handling of known-count mixed batches

Status: Rejected for adoption (2026-11-16). Three variants implemented and
reverted; two crashed, one passed correctness but regressed every target.

## Attempt

Give mixed batches a **known count** (walk the list once at flush time to
record its length, publish `count = n, pure = 0`), then make the census treat
known-count mixed batches without walking them:

1. **Wholesale republish with fresh header** — republish as-is during the
   rebuild. Crashed: the loop still released the original header, and pure
   chains raced the withhold bookkeeping; header lifecycle fixes (skip the
   release, re-push the original header objects) still corrupted, because
   blocks of *dead* spans and blocks in *newly purged* pages must be
   intercepted per block, which a wholesale republish cannot do.
2. **Count-skip + republish + purge disabled in that mode** — in regroup-only
   mode, mixed batches are not walked and not counted (their spans provably
   look alive: undercount is safe), page purge is disabled for the mode, and
   the rebuild republishes mixed batches as unknown-count batches. Passed
   v2/jit basic+stress (5×5). Measured: mt 8M RSS 8716 KB (HEAD ~3100),
   trim after_free 161440 (HEAD ~153000), locality 71.4 ns — unknown-count
   batches in central make refills install span-blind lists and the missing
   purge raises RSS.
3. **Pure-only death proof, all modes** (mixed batches never counted; spans
   die only when fully covered by span-pure batches) + republish: correctness
   passed, but trim after_trim regressed to 127596 KB (the death proof is too
   weak to unmap anything), pages degraded to 509, macros 2–3× slower.

## Decision

Exact per-span attribution of mixed-batch blocks is load-bearing for the
death proof, the purge withhold, and the regroup itself. Any scheme that
avoids the per-block walk either weakens release to the point of regressing
RSS/trim/pages or is unsafe. Reverted. The honest conclusion: mixed-batch
walks are removable only by making batches span-pure at the source (see
ADR 0062) or by restructuring hand-out so mixed batches do not form.
