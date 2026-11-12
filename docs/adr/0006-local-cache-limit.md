# 0006 — Per-thread local cache limit of 256 blocks, size-bounded

*Status: Accepted. Provenance: Reconstructed (2026-11-12, from README
§2.6 and `kClassLimit` in `src/fm_core.h`).*

## Context

The local-cache limit trades memory retained per thread against how
often the flush branch is taken and mispredicted.

## Decision

Default limit 256 blocks for classes up to 256 B, scaled down for
larger classes (down to 4 for the largest) so a thread never holds an
unreasonable number of *bytes* in one class. The choice was measured on
the churn workload (README §2.6): branch mispredicts fall monotonically
from 0.907 % at limit 16 to 0.195 % at 512, with instruction counts
flattening after 256 (16.78M at 256 vs 16.73M at 512), so 256 sits at
the knee.

## Consequences

`bench/sweep.sh` re-runs this sweep. The retained-memory cost of the
caches is one of the two reasons v2's peak RSS exceeds glibc's on real
programs (see 0010).
