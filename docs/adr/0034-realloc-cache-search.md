# 0034 — Wider large-cache search on realloc growth

*Status: Accepted as an opt-in prototype; default promotion rejected
(2026-11-13). Container evidence is indicative only; downstream VM
re-measurement owns the final adoption verdict.*

## Context and decision

Read round 6's backlog and ADRs 0031/0033 before implementation.
Mapping extension failed in round 6. Its surviving observation was that
the cache searches only the request's bucket and its successor, missing
larger retained mappings that could serve realloc growth.

`FM_REALLOC_CACHE_SEARCH=1` searches the remaining buckets in ascending
capacity order for plain large realloc growth, after the existing
fits-in-mapping check. Each bucket still examines at most eight entries.
Default is 0 in every FM_PROFILE; the existing parser clamps the boolean.
Ordinary malloc/calloc, aligned realloc and small-class realloc retain
their existing allocation paths. The per-thread byte budget, usable-length
check (excluding mapping head slack), unlink/accounting, and copy/free
ordering are unchanged. No mappings are extended or moved by the kernel.

## Measurements

All numbers here are **cloud-container indicative measurements**, not
comparable to the project's reference VM. The first isolated rgrow pair
(200 rounds / 2,200 operations) was v2 0.189635 s off / 0.197973 s on;
JIT 0.193554 / 0.206199. There is no synthetic win in that pair.

Real workload: the repository's unchanged Lua churn script, run with the
installed `texlua` interpreter (not the reference VM's Lua executable).
Three alternating v2 pairs, seconds / peak RSS in KiB:

| pair | off | on |
|---|---|---|
| 1 | 0.372999 / 26240 | 0.349009 / 26496 |
| 2 | 0.291537 / 26624 | 0.295093 / 26624 |
| 3 | 0.313871 / 26496 | 0.336505 / 26752 |

Checksum **19466078** in every run. Median time 0.313871 → 0.336505 s;
median peak 26496 → 26624 KiB. A later syscall-count pair on the hardened
build saw mmap 548 → 559 and munmap 538 → 550 (includes loader/interpreter
startup; not a cache-hit counter). There is no demonstrated real-workload
benefit. A wider search can also pin a much larger capacity to a small
request. Keep the requested prototype testable; reject default promotion.

## Validation and consequences

`bench/realloccache.c` seeds an 8 MiB cached mapping then grows a 128 KiB
block to 256 KiB. Pointer identity proves the distant bucket was used only
with the flag on. All preserved bytes are checked; subsequent calloc
checks every byte for zero; trim drains the cache. Both v2 and JIT pass
both settings. The full round-7 battery also combines this flag with the
idle timer and concurrent purge stress. `bench/round7.sh` reproduces it.

This is opportunistic recycling, not a claim that widening search eliminates
Lua's one-way growth trail: a predecessor smaller than the current request
cannot serve that request. A suitable larger mapping must already be cached.
