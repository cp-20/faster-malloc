# 0035 — Purge under contention: make census and batch ownership explicit

*Status: Accepted correctness fix (2026-11-13). Performance costs are
indicative in this cloud container; final performance adoption is subject
to downstream reference-VM re-measurement.*

## Evidence and scope

ADR 0030's intersect/start-count proof still holds when blocks in another
thread's local list are invisible. The proof also requires exclusive
ownership of census state and safe ownership transfer of batch headers.
Inspection found three violations around that proof:

1. All censuses shared non-atomic scratch counters, including the allocation
   failure flag used in purge accounting. Concurrent classes raced too.
2. Two censuses could detach disjoint batches from the same span and race
   on its purged mask, withheld count and age marks; reclaim requires an
   exclusive view of those fields, not just disjoint free-block lists.
3. A central pop read `b->next` before successfully claiming the header.
   Another pop/census could recycle that header and overwrite `next` through
   a thread-local batch pool (a data race and an ABA lifetime hazard).
   A span's `dead_mark` also raced between refill and census.

These are code-audit findings, not a claim of reproduced user-data loss:
ten runs of the expanded harness against the pre-fix debug library passed.
Absence of a crash does not make the C data races valid.

## Decision

Serialize censuses with one mutex, serialize central push/pop/detach with
another short-held mutex, in that order when both are required. Never hold
the central mutex through a census walk, mmap, sorting or batch recycling.
This protects stale readers before recycling and makes central depth updates
consistent with detaches. Census result counters become thread-local because
automatic cadence reads them *after* census ownership is released; empty
censuses reset their results too. `dead_mark` becomes atomic. The page
intersect/start-count rules and withheld-block geometry do not change.

This deliberately supersedes ADR 0005's lock-free central implementation.
A hazard-pointer/epoch scheme would need a larger lifetime and fork design;
a plain mutex on already amortised slow paths is the smallest complete fix.
Thread-local malloc/free fast paths still take no locks. ADR 0036 registers
fork prepare/parent/child handlers that quiesce and release both locks.

## Harness and measurements

`bench/stress.c` now runs eight purge workers alongside four shadow-model
realloc/free/calloc/alignment workers. Each purge worker runs 24 rounds of
2,048 fully touched 896-byte allocations, leaves live islands every sixteen
blocks, trims, refills the class with islands still live, verifies all bytes,
grows the islands to 1,792 bytes and trims again. Those sizes straddle page
boundaries. Other workers retain local lists while trims race with activity.
The harness RNG is now thread-local (the old shared RNG itself raced), a
successful iteration now releases its slot reservation (previously, once
all slots filled, the workers mostly skipped rather than churned), failed
realloc releases its reservation too, final live slots are verified, and
calloc checks its full
allocation rather than just the first dimension.

One traced run before the additional refill phase issued 7,350
MADV_DONTNEED calls: this exercises actual partial-span purge, not just
whole-span unmapping. Successful debug runs also use age 0 / minimum one
purge page to intensify reclaim, with v2 and JIT census assertions enabled.
The final round document records the full battery and final trace count.

An alternating, same-compiler O3 comparison against round-6 v2 used
`bench/mt MODE 2000000 8`, three runs each:

| mode | baseline median (range), s | hardened median (range), s |
|---|---|---|
| local | 0.061144 (0.053883–0.071806) | 0.075874 (0.048016–0.090201) |
| cross-free | 0.052916 (0.052026–0.059825) | 0.060605 (0.057068–0.068089) |

These cloud measurements are **indicative only**, not reference-VM numbers.
Local ranges overlap widely; cross-free shows a possible ~15% slow-path
cost. Do not claim performance neutrality. Correctness ownership is required;
measure downstream before choosing a more complicated reclamation scheme.
