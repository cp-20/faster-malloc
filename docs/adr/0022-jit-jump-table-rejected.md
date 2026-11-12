# 0022 — JIT jump-table dispatch: implemented, measured, rejected

*Status: Rejected after measurement (2026-11-12). Recorded
contemporaneously. The implementation (by Codex, reviewed and
verified in-tree) was reverted the same day; this ADR is the
record.*

## Context

README §6's long-standing JIT idea: replace the generated
dispatch's sequential range tests (one lea/cmp/jbe per hot class,
up to 8) with a branchless jump table indexed by `size >> 4`, so
reaching a leaf — or the generic fallback — costs one indirect
jump instead of up to eight predicted branches. The concentration
heuristic (ADR 0008) stays as the safety net: distributions with
no dominant class step aside entirely.

## Implementation notes (for whoever reprices this)

The dispatch is exact, which is the part worth keeping on record:
for every `0 <= s <= 32768`, `fm_class_of(s) == kClassBig[(s+15)>>4]`
(kClassBig is the identity in the arithmetic range), so a
2049-entry table with `table[b] = leaf(kClassBig[b])` for hot
classes and the generic entry elsewhere dispatches identically to
the class computation. A 64-bit `cmp/ja` on 32768 guards the
index. The table template lived beside the range template in
jit_stubs.S (same leaf structure, own generic target), the
generator verified patch-point layouts as the range path does,
and any failure fell back to the range template. A size sweep
(0..40000, pattern-verified, under a 3-leaf install) and the full
battery passed; concentrated workloads installed the table,
diverse ones still stepped aside.

## Evidence

Purpose-built probes (min-of-5, taskset) and macro scenarios
under default JIT settings, table vs the range-test build:

| workload | leaves | range | table |
|---|---|---:|---:|
| multihot (50 % one class + tail) | 1 | **36.3 ns/op** | 41.2 ns/op |
| multihot2 (4 classes ≈12 % each + tail) | 4 | 45.6 ns/op | 45.7 ns/op |
| macro kvstore | default | **604.9** | 658.2 |
| macro jsondoc | default | **234.5** | 241.1 |
| macro compiler | default | 38.0 | 38.0 |

The table never wins. At one leaf it loses ~13 %; at four leaves
it ties; on the real macro workloads it loses 3–9 %.

## Why

The range tests it replaces are register-only compares with
well-predicted branches — for the installs the picker actually
produces (it stops adding classes at 50 % coverage, and the top
class must itself be ≥ 50 % at default settings, so *one* leaf is
the common case) they cost almost nothing, and exact-size
narrowing makes the common test a single compare against a
constant. The table pays a fixed toll on *every* call instead: a
dependent load from a 16 KiB table (another cache line in the
working set) plus an indirect branch whose target alternates
between leaf and generic on mixed traffic — the worst case for
the branch-target buffer. The regime where a table should win
(many leaves, traffic spread across all of them) is precisely
the regime the concentration heuristic avoids because the static
arithmetic dispatch beats *both* generated forms there
(multihot2: v2 static 31.8 ns/op vs 45.6/45.7 for the two JIT
forms, under a forced low min-share).

## Decision

**Rejected.** The range-test template stays. The idea is not
wrong in principle — on a workload with many genuinely hot
classes and a predictor-unfriendly size stream it could still
pay — but that workload is one this JIT deliberately declines to
specialise, and inside the envelope it does serve, the table is
a cost, not a saving.
