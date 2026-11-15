# 0068 — Chain-free bump hand-out

Status: Rejected for now (2026-11-16); partially validated, reverted before
commit because its census interaction regressed the locality metric, and the
attempted companion fix (ADR 0069) did not land.

## Attempt

`fm_refill_class` builds a 256-block chain for the TLS list and every setup
malloc then reads an untouched link line: the chain build costs one
first-touch write per block and every pop one first-touch read — the
allocator's main share of setup-phase D1 misses (measured: `malloc` ~360k
D1mr over a locality run). Instead, reserve the bump range as a hand-out
cursor (`fm_bump_ptr`/`fm_bump_end` already exist): `fm_malloc` hands out
`cur += bs` without touching block memory; the span is fully reserved, so no
remainder batch is published and no link words are written until blocks are
freed.

Two priority orders were tried:

- **bump before central** (fast-path check): set-up fast, but after a flush
  the TLS list empties while the bump range is still non-empty, so
  allocations bypass central recycling and fragment across fresh spans —
  jq A 0.24–0.28 s (0.17), jq B 0.77–0.91 s (0.57–0.64), mixed Ir +5.4.
  Rejected.
- **central first, bump inside refill**: Ir improved broadly (churn64 57.6→
  54.6, churnmix 68.9→66.7, retain 65.9→62.8, cross 64.3→62.7 — retain/cross
  now effectively match or beat jemalloc 80.1/81.8 and approach mimalloc),
  RSS improved broadly (server 9412→6496 KB, jsondoc 3144→2608,
  game 2144→1888, mt 4M/8M 2844/2800→2636/2680), mt time 0.08/0.19 s, trim
  parity — **but** locality regressed: ns 42–44 (32–35), D1 2,481,504
  (2,217,438), loc3 1,241,189 (1,121,321).

The locality regression is mechanical: with no remainder batches published,
*every* central batch is a mixed flush batch, so the census walks the whole
stack (fm_trim_class D1mr 688,617 → 1,014,726) — the pure-batch wholesale
accounting that made the census cheap is gone.

## Decision

The hand-out itself is good (its Ir and RSS wins are the largest seen this
session) but only in combination with a census that does not need pure
remainder batches — the fused one-pass census of ADR 0069 was that
companion, and it did not land. Reverted pending a working fused census; do
not ship the hand-out alone because it trades locality for Ir/RSS.
