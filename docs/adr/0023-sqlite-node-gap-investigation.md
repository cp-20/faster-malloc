# 0023 — The sqlite and node "gaps": profiled; no allocator defect found

*Status: Accepted as an investigation record (2026-11-12). No code
change resulted. Recorded contemporaneously.*

## Context

Round 3's app matrix had v2 behind glibc on two cells: sqlite
10.95 s vs 10.37 s (1.056×) and node 1.77 s vs 1.58 s (1.12×).
Round 4's brief was to profile what these workloads actually do
and close the gap with a general allocator change if one exists —
or prove the gap is a harness/runtime artefact.

## Method

A request-stream interposer (`patprobe`: counts malloc/free/
realloc by size class, realloc grow/shrink/same, alignment
requests, threads — the stream is a property of the workload, not
the allocator), two purpose-built realloc-growth probes, and
repeated matrix sessions with the unchanged v1 as drift control.

## sqlite (speedtest1 --size 30)

Profile: **single-threaded**; 219k mallocs dominated by small
sizes (16–192 B) plus a 4–8 KiB band (38k — the page cache); 41k
reallocs of which 34.5k grow (+50.1 MB total growth); no calloc,
no aligned allocation, no `malloc_usable_size` calls. Nothing in
the stream is something v2 handles pathologically: small classes
are its home ground, and the fits-check (ADR 0018-A) already keeps
same-class realloc growth in place.

The growth probes split the pattern in two:

- *Interleaved growth* (64 buffers growing together — the shape
  of SQLite's actual mix, where a growing buffer is never alone):
  v2 is **6–7× faster than glibc** (67–78 vs 464–540 ns/op),
  because glibc copies on every cross-size grow while v2's
  alloc/copy/free path is simply cheaper.
- *Single growing buffer*: glibc can extend the chunk in place at
  the top of its arena and copy nothing — a real structural
  advantage of the arena design that the span design has no
  analogue for (there is no O(1) way to ask "is the physically
  next block free?" of a free-list allocator). On this VM the
  probe is page-fault dominated and cannot price it: glibc itself
  swung 13.8 → 220.7 ns/op across identical reps.

And across sessions the gap will not sit still: round 3 measured
1.056× for glibc; this round's sessions measured v2 *ahead*
(0.92× in the subset run; the final full-matrix run is in the
round-4 log), with the v1 control steady at 0.96–0.97× in both.
A 5 % effect that flips sign between afternoons, on a VM whose
absolute times drift 5–8 % between sessions, is not an allocator
property.

## node (churn.js)

Profile: 168k mallocs and only 403 reallocs across the whole run,
7 threads, 31k `malloc_usable_size` calls, 94 aligned requests.
The arithmetic closes the case by itself: 168k calls cannot
produce a 0.1–0.2 s delta — even a (wildly implausible) 100 ns/op
of allocator disadvantage sums to 17 ms. The observed delta has
to be drift or second-order effects (v2's higher peak RSS means
more page faults inside a V8-heap-dominated run), and the
sessions oblige: +12 % (round 3), +5 % (round-4 subset), with the
unchanged v1 swinging +2 % → +14 % across the same sessions.
V8's own heap management is the workload; the libc allocator is a
bystander with a noise-level signal.

## Decision

**No change.** There is no general allocator defect behind
either gap that this VM's measurements can support acting on:
sqlite's stream plays to v2's strengths (and its one structural
disadvantage — in-place top extension — is unpriceable here and
has no span-design analogue), and node's delta is arithmetically
too large to be allocator time at all. Both cells stay on the
watch list: if a future round runs on quieter hardware, the first
things to re-measure are these two, with the interposer profile
above as the baseline of what the workloads request.
