# 0069 — Fused census pass (count + chain + cover in one walk)

Status: Unresolved, reverted (2026-11-16). The mode-0 census walks the
central stack three times (count mixed blocks, cover candidate spans'
pages, rebuild/regroup). This attempt fused them into one pass and failed
on a block-accounting invariant; the work is reverted and the design notes
preserved here for the next attempt.

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

## Next attempt

Debug the identity bug with per-block instrumentation (`fm_census_add`
return vs chain write), and consider re-deriving `e` from `fm_census_find`
unconditionally for every block (cost: one extra hash probe per block —
still one walk total, strictly cheaper than today's three). The design
remains attractive: it is what makes ADR 0068's hand-out viable, and it
removes two of the census's three walks for mixed-heavy stacks.
