# 0069 — Fused census pass (count + chain + cover in one walk)

Status: Unresolved, reverted twice (2026-11-16). Second attempt isolated the
earlier "identity bug" to broken instrumentation, passed every block-level
invariant, measured the best D1/locality numbers of the session, and was
reverted only because the `FM_DEBUG` canary checks still abort with all
invariants green. Design notes and the verified instrumentation are kept
here.

## Design

One pass over mixed batches: per block, `fm_census_add` (register + count),
chain the block onto its span's census entry (`e->ahead/atail/acnt`), and
`fm_pg_cover` (page coverage for every span — candidacy is decided later, so
cover all). Terminate every chain after the pass. The rebuild then:

- splices span-pure batches wholesale (all of them; dead spans' chains are
  dropped at assembly, which is where a chain is terminated anyway);
- releases mixed batch headers without walking (blocks already chained);
  blocks the census dropped park in an `unreg` side chain and join the
  residue;
- runs a single withhold pass per span with newly purged pages
  (`pg_new_mask`), re-chaining the span's whole chain and removing blocks
  that start in those pages (`pg_skip_cnt`), instead of walking all mixed
  batches;
- assembles as before, dropping dead spans' chains whole.

## What was fixed during the attempt

- `fm_census_grow` re-hashes entries and previously reset chain/page state:
  the pass now holds an entry across a grow, so grow was changed to copy the
  entry whole (`*e = old[i]`) and the walk re-looks-up its entry after a
  grow (`c->grew` flag).
- Chains must be terminated after the walk (the withhold pass and the
  splice path walk/append from them).
- The withhold pass must decrement `acnt` on *skip* (survivors keep the
  count) — the first version decremented on keep, producing
  `acnt == skip`-shaped accounting.
- Pure batches of purge-flagged spans must be spliced wholesale and let the
  withhold pass do all skip counting (the per-batch walk double-counted).

## Where it stopped

With all of the above fixed, `FM_DEBUG` accounting failed:

```
[fm] BLOCK ACCOUNTING chained=12+res=0 vs walked=244+pure=12
```

232 of 244 walked mixed blocks were neither chained nor parked as unreg —
i.e. `fm_census_add` succeeded but the walk's chaining bookkeeping lost
them — while per-entry chain-length checks (walk length == `acnt`) passed at
the same time, which points at an entry-identity bug in the fused walk
(re-homing, hash slot aliasing, or a stale entry pointer surviving the
grow check) rather than at chain construction. Unresolved; reverted.

## 2026-11-16 second attempt: identity bug isolated to instrumentation; new blocker

Rebuilt the full design on `cb178ee` (fused count+chain+cover walk, chain
termination after the walk, grow copies entries whole with a `c->grew`
re-home, pure batches spliced wholesale with dead spans dropped at assembly,
a single withhold pass per purged span that re-chains the span's chain and
removes `pg_new_mask` blocks, assemble-side dead-chain drop). Under
`FM_DEBUG`, per-block verification now passes:

- `fm_census_add` return always matches the block's span (ADD/CHAIN
  MISMATCH never fires); chain lengths equal `acnt` at walk end, pre-withhold,
  and assembly; the pre-withhold block accounting (`chained + residue +
  dead_dropped == walked + pure`) balances on every census; a per-block
  seen-set finds no duplicate blocks inside chains or residue; the
  end-to-end conservation probe (`after + dead + newly-withheld − reclaimed
  == TLS + detached-stack blocks at detach`) balances on every census.

With all invariants green, the **production** build passes the full battery
(v2/jit test_basic+stress 20/20) and posts the best census-related numbers
measured so far: cachegrind D1 locality 2,217,438 → 1,871,606 (−16%),
loc3 1,121,321 → 950,426 (−15%), locality 24–27 ns/op (from 32–74), Ir/op
unchanged (retain 65.9 → 65.0, others identical), trim/macros/mt/jq parity.

**New blocker:** the `FM_DEBUG` canary checks still abort (`CORRUPT HEAD`)
with canaries holding either zero (a MADV_DONTNEED'd page) or the app's user
data. Canary=0 fits re-carved withheld blocks whose canary slots were zeroed
by madvise — rewriting canaries in the reclaim loop fixed only part of it.
User-data canaries mean a block was handed out while still in another list,
but the two membership checks that would catch it (block already in the TLS
list at free; duplicate block across rebuilt chains and residue) never fire,
and every structural invariant above passes. The remaining suspect is an
interaction that survives all block-level invariants — e.g. a batch whose
recorded count still exceeds its live membership after a withhold pass, or a
stale batch published across censuses. Unresolved; reverted.

## Next attempt

The instrumentation layer is now trustworthy (all invariants green), so the
next session should: (1) record every batch publish (`fm_publish2`,
regroup push, residue) with its chain length and verify count == length at
pop time to catch a stale/inflated batch header; (2) verify `sp->withheld_cnt`
against a recomputation from `purged_mask` at every census entry; (3) if both
hold, add a global free-set membership check (hash of all blocks in all
lists) to catch the double-hand outside the census. The measured upside
(−16% D1, locality 24–27 ns) is the largest single lever left for the
locality gap.
