# 0014 — Free-list shaping: span-grouped recycling into a per-thread stash

*Status: Rejected after measurement (2026-11-12). Recorded
contemporaneously. The code lived on the round-2 branch (commit
cd32f1d) and was removed by the commit that adopted ADR 0015
instead; it remains in the branch's history.*

## Hypothesis

v2 publishes a flushed free list in free order, so a batch mixes
blocks from every span the freeing thread touched, and the next
refill sprays allocations back across all those pages. ADR 0010 and
README §6 name this the biggest known gap to mimalloc, whose
per-page free lists keep reuse on one page at a time.

To make the gap measurable without cachegrind (unavailable on this
VM), a new `locality` macro scenario was written first: free a
32,768-block working set (64 B blocks, a 528-page pool) in scrambled
order, reallocate an eighth of it, and count the distinct 4 KiB
pages the burst lands on. It is an address count, not a timing, so
it is bit-stable across runs. Baselines: **v1/v2/jit 512.9 pages,
glibc 640.5** (glibc's pool is bigger: 80 B chunks), floor 64. Every
allocator sprays uniformly; nobody concentrates.

## Change tried

Flush-side grouping: `fm_flush_bin` walks the detached list, groups
blocks by span into a per-thread *span stash* (16 slots per class,
groups merge by span across flushes, 8 MiB per-thread byte budget),
publishes overflow groups span-pure, and refills drain the stash's
largest group first. Batches learned to record their length and
span-purity along the way (that part is kept — see ADR 0015).

## Evidence

**Round 1 — unbounded walk + eager span release.** A completed span
(a group covering all its blocks) was unmapped on the spot.
Locality improved immediately (512.9 → ~105 pages), and the trim
scenario's RSS after free fell from 161.6 MB to 31.2 MB. But the
probe suite (taskset, min-of-3, same minutes) collapsed:

| scenario | before | round 1 |
|---|---|---|
| churn 8 B | 7.07 | 64.30 |
| churn 64 B | 10.89 | 21.11 |
| batch 64 B | 10.69 | 66.54 |
| hold 64 B | 12.23 | 66.49 |

**Bisection.** Two independent causes, found by disabling one
mechanism at a time:

1. *Eager release* explains batch/hold. With shaping kept but
   completed spans published instead of unmapped: batch 14.34,
   hold 15.94. A span that completes every round costs an munmap
   plus an mmap plus a page-fault refill every round; on this VM
   syscalls and faults are the most expensive operations there are.
   glibc does not return this memory during a run either — it waits
   for `malloc_trim` (see ADR 0015) — and the measurement says
   glibc is right.
2. *The walk itself* explains churn. A flush detaches the *whole*
   local list, which is mostly the untouched remainder of the last
   refill's batch, not blocks freed since. Single-block churn
   therefore walked a 3,837-block list (one 16 B span) every 256
   operations — ~15 extra pointer chases per op to recycle one
   freed block. Publishing remained O(1) in the old design for a
   reason; the walk has to be bounded by the frees it serves.

**Round 2 — bounded walk, no eager release.** Shape only a prefix
of the detached list (`kClassLimit + 32` blocks — frees always sit
at the head; the suffix is published unshaped), with a single-span
fast path that hands a uniform prefix over without relinking it.
Probe recovered most of the ground (churn8 8.84, churn64 8.73,
batch 15.43, hold 17.68–20.10) and locality reached **67.8 pages**
— within 6 % of the 64-page floor.

**Full suites, final shaped build vs pre-round-2 build** (ratios to
glibc within each session; the unchanged v1 serves as drift
control and was stable):

| workload | v2/glibc before | v2/glibc shaped |
|---|---|---|
| compiler | 0.49 | 0.58 |
| jsondoc | 0.76 | 0.84 |
| kvstore | 0.83 | 0.89 |
| game | 0.28 | 0.31 |

The micro suite agreed (batch 8.56 → 15.48, hold 10.96 → 20–32
depending on runner, rand 86.9 → 217.0, touch 6.75 → 17.47,
threads 5.45 → 11.75 ns/op), and the server scenario's peak RSS
*rose* (11,296 → 16,928 KB) from stash retention across threads.

## Decision

**Rejected.** The mechanism works — the locality metric proves the
grouping does exactly what it claims (513 → 68 pages) — but the
price is at least one extra pointer chase per recycled block, and
on every timed workload the tax exceeded the refund. The likely
reason is instructive: in the realistic workloads the free order
*already* clusters by span (objects allocated together are freed
together), so batches published in free order are mostly span-pure
to begin with and there is little spray left to remove. The
workload where spraying is real — fully scrambled frees — is also
where grouping is most expensive. On the author's machine, with
cachegrind D1 counts, the trade might price differently at larger
working sets; the `locality` scenario now exists to ask the
question with, and refill-side regrouping (walk a popped batch
once, on the allocating thread, instead of at flush) remains an
untried variant. Kept from the experiment: batch length/purity
records (free to produce, needed by ADR 0015), the `locality` and
`trim` scenarios, and this ADR.
