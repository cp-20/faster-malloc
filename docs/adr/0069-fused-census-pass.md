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

## 2026-11-16 third attempt: census logic formally verified, bug isolated upstream in the free path

The full fused design was rebuilt with an exact, formal-verification-grade
instrumentation suite (per-span in-use bitmaps with exact hand-out/free
membership, double-push checks, install-chain verification, span-overlap
registry, per-census multiplicity and conservation invariants). Findings:

1. **All census-internal invariants now pass** on every census of
   test_basic: exact multiplicity (`seen + dead + newly-withheld - reclaimed
   == walked + pure`), conservation (stack == live chains + residue), no
   duplicate blocks across chains/residue, no in-use blocks in installed
   batches. The earlier "identity bug" and the block-accounting failures
   were instrumentation artifacts (dead entries with `acnt==0` skipped by
   the first guard; a wrong `dead_drop` term double-counting dropped pure
   batches; a residue merge that double-counted entries in `live`).

2. **The real defect lives in the free-path hand-off, upstream of the
   census.** The exact bitmap repeatedly proves that a *flush-published*
   batch consists entirely of blocks that are currently handed out
   (`IN-USE BLOCK ON STACK ... publisher=1 qidx=0 chain_len=12
   inuse_in_chain=12 canary=<app fill>`), i.e. the TLS list contained
   handed-out memory at publish time. Every list-mutation site is
   instrumented (double-push on free, install-chain verification with
   count-vs-length, non-inuse check on free) and none fires - so a
   hand-out advances `fm_head` past a block whose predecessor's link was
   left pointing at it by an earlier state of the list, and the next
   publish's chain walk follows that stale interior link into handed-out
   memory. The census then counts handed-out blocks as free, can prove a
   span dead while it is in use, and unmaps it: the corruption users see.

3. Root-cause fixes found and applied along the way (kept for any future
   round): function-local `static` pools inside inlined header functions
   must be hoisted to file scope (each inlined copy silently aliased
   entries), and per-span side state must be dropped when a span is
   unmapped (a reused VA inherits the previous span's bitmap otherwise).

Blocked on identifying which hand-out leaves the stale interior link.
Instrumenting the *pop* to verify the popped block is the list head (i.e.
the TLS list is LIFO-consistent) is the next diagnostic; a non-LIFO pop
would mean a list walk and head/tail update disagree somewhere between
install, push and pop.

The measured upside of the fused census is unchanged and large: locality
D1 2,217,438 -> 1,871,108 (-16%), loc3 1,121,321 -> 950,177 (-15%),
locality 22.9-26.4 ns/op (from 32-74), mt/macros/trim/jq parity or better,
production v2/jit batteries 20/20 pass throughout. It stays rejected until
the free-path aliasing is fixed, because the FM_DEBUG suite (10/10
test_basic failures with the exact bitmap) is the correctness gate.

