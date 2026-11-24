# 0084 — Move large realloc growth with mremap

Accepted 2026-11-25.

## Context

ADR 0080 reserves one extra request when a large block grows, so a single
geometric step usually fits in place.  When it does not, `fm_realloc_slow`
allocates a new block and `memcpy`s the old payload: at the peak of the
copy both buffers are resident.  Any parser or builder that doubles a
multi-MiB buffer pays that at every step, which is what CPython's
`json.dumps`/`json.loads` does.

Local measurements on the real-suite fixtures:

| case | glibc | v2 (copy) |
|---|---:|---:|
| `grow` 1 -> 128 MiB, one buffer | 132.6 MiB | 139.7 MiB |
| `py_pipeline.py` JSON half only | 134.8 MiB | 142.0 MiB |

## Decision

In the large-block growth path, reserve a destination mapping, trim it to a
64 KiB-aligned base, and move the old mapping onto it with
`mremap(MREMAP_FIXED | MREMAP_MAYMOVE)`.  The kernel moves page tables
instead of copying bytes, and the old pages are released immediately.  If
either the reservation or the move fails, the existing copy path runs
unchanged.

The 64 KiB alignment is not cosmetic: `free` and `malloc_usable_size` find
the span header by masking the pointer with `SPAN_SIZE - 1`, so a moved
block has to keep a 64 KiB-aligned base.  `mremap` alone only guarantees
page alignment, hence reserve-and-trim.

```c
need = align_up(off + size + HEADER_SIZE, 4 KiB);
dst  = align_up(mmap(NULL, need + 64 KiB, PROT_NONE), 64 KiB);
mremap(sp->map_base, sp->map_size, need, MREMAP_FIXED | MREMAP_MAYMOVE, dst);
```

## Evidence

* `rtest`: 8 threads x 200 rounds, each doubling 512 B -> 512 MiB with a
  byte-pattern check after every step, passes on glibc, main and this
  branch.
* `test_basic`, `bench/stress`, `valgrind --tool=memcheck` stress and
  `mtrepeat` (5 runs x 2 libs) pass.
* `grow` peak 139.7 -> 132.8 MiB (glibc 132.6 MiB).
* `py_pipeline.py` JSON half 142.0 -> 138.0 MiB (glibc 134.8 MiB).
* git rev-list is unchanged (23.9 MiB), so its gap is not realloc copies.

## Risks

* Two extra syscalls per growth (one reservation, one `mremap`); growth is
  already a slow path and the copy it replaces is O(bytes).
* Under address-space pressure the reservation can fail and the code falls
  back to the copy.
* A moved block leaves its 4 MiB slab; the old pages are unmapped, which is
  the intended effect, but the slab's remaining tail stays as it was.
