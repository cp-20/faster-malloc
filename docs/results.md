# Raw benchmark output

This is the verbatim output of `bench/suite.sh` on the machine the allocator
was developed on (AMD Ryzen 9 6900HS, WSL2, shared and heavily
virtualised — see the note on measurement noise in the README).

The instruction counts are exact and reproduce bit-for-bit. The wall-clock
numbers are not: they moved by up to 10% between two runs of the same
suite, which is why the instruction counts are the primary metric.

```
$ REPS=3 ./bench/suite.sh
```

## Section 1 -- instruction counts (callgrind)

Exact and reproducible. These are whole-program instruction counts, so
they include the workload's own instructions (about 8 per iteration) and
the ~140K instructions of dynamic-loader startup.

| workload | glibc | tcmalloc | jemalloc | mimalloc | v1 | v2 | jit |
|---|---|---|---|---|---|---|---|
| churn 300000 @64 B | 42748472 | 30553633 | 24908000 | 34154391 | 21976432 | 15577790 | 15167516 |
| churn 300000 @8 B | 41848450 | 30550692 | 24767483 | 31832688 | 21994826 | 15596184 | 15185910 |
| churn 200000 @1 KiB | 28548499 | 22452459 | 18907798 | 30258792 | 16284566 | 11641494 | 10548861 |
| churnmix 200000 | 30949251 | 24874395 | 19851201 | 25858544 | 17163086 | 12903211 | 13228924 |
| mixed 150000 | 30553572 | 23844564 | 19331839 | 14292674 | 16239827 | 13071022 | 13347837 |
| retain 150000 | 23026111 | 19318199 | 14913208 | 9463985 | 11896198 | 8673797 | 8556475 |
| cross 150000 | 23176552 | 19496439 | 14985103 | 9610493 | 12044714 | 8823757 | 8706435 |

## Section 2 -- cache and branch behaviour (cachegrind)

churn, 64 B:

| allocator | Ir | D1 misses | LL misses | D1 miss rate | branches | mispredict % |
|---|---|---|---|---|---|---|
| glibc | 42750061 | 1264 | 1044 | 0.003% | 5728179 | 0.065 |
| tcmalloc | 30562108 | 15984 | 7995 | 0.052% | 7401912 | 0.207 |
| jemalloc | 24916995 | 15970 | 8258 | 0.064% | 2773781 | 0.621 |
| mimalloc | 34156075 | 1815 | 1407 | 0.005% | 4922129 | 0.194 |
| v1 | 21978049 | 1465 | 1103 | 0.007% | 2249148 | 0.325 |
| v2 | 15579431 | 1731 | 1108 | 0.011% | 2438431 | 0.219 |
| jit | 15169178 | 2416 | 1682 | 0.016% | 2820815 | 0.236 |

churnmix, 8 sizes:

| allocator | Ir | D1 misses | LL misses | D1 miss rate | branches | mispredict % |
|---|---|---|---|---|---|---|
| glibc | 30950840 | 1268 | 1046 | 0.004% | 3828325 | 0.098 |
| tcmalloc | 24882870 | 16049 | 7996 | 0.064% | 6305338 | 0.244 |
| jemalloc | 19860156 | 16026 | 8272 | 0.081% | 1978025 | 0.883 |
| mimalloc | 25860226 | 2240 | 1412 | 0.009% | 3390207 | 2.928 |
| v1 | 17164703 | 38326 | 1102 | 0.223% | 1519257 | 0.459 |
| v2 | 12904852 | 30533 | 1108 | 0.237% | 1646477 | 0.352 |
| jit | 13230583 | 5857 | 1675 | 0.044% | 2516275 | 0.374 |

mimalloc's 2.9% branch mispredict rate on the diverse workload is the cost
of the range check at the end of its fast path — the same mechanism the
JIT's dispatch uses, and the reason the JIT needs a concentration
heuristic before it specialises.

## Section 3 -- real programs

Wall clock in seconds (minimum over several runs) and peak RSS in KiB.
The `cfrac` and `espresso` binaries are the SPEC CPU2006-derived ones from
the mimalloc benchmark suite; `barnes` is the n-body program.

| allocator | cfrac (s) | cfrac RSS | espresso (s) | espresso RSS | barnes (s) | barnes RSS |
|---|---|---|---|---|---|---|
| glibc | 4.18 | 3276 | 3.90 | 3392 | 2.87 | 58364 |
| tcmalloc | 3.30 | 8692 | 3.37 | 9584 | 2.93 | 64152 |
| jemalloc | 3.39 | 5340 | 3.50 | 5864 | 3.54 | 60896 |
| mimalloc | 3.50 | 3380 | 3.40 | 3688 | 3.13 | 58544 |
| v1 | 3.25 | 3412 | 3.49 | 8980 | 3.12 | 58472 |
| v2 | 3.16 | 3796 | 3.50 | 8956 | 2.85 | 58452 |
| jit | 3.19 | 3692 | 3.41 | 9044 | 2.87 | 58448 |

## Section 4 -- multi-threaded

8 threads, 2M operations each, seconds.

| allocator | local alloc+free | cross-thread free |
|---|---|---|
| glibc | 0.035 | 0.120 |
| tcmalloc | 0.028 | 0.132 |
| jemalloc | 0.029 | 0.089 |
| mimalloc | 0.045 | 0.071 |
| v1 | 0.027 | 0.048 |
| v2 | 0.021 | 0.049 |
| jit | 0.026 | 0.054 |

## Reproduction on other hardware

The CI workflow on this repository re-measures the instruction counts on
GitHub's runners (a clean, uncontended machine), which is a useful
independent check:

```
glibc                7648483 instructions
libfastmalloc2.so    5305540 instructions
libjitmalloc.so      5320026 instructions
```

(100000 churn iterations. On an uncontended machine the JIT's profiling
warmup is a larger fraction of a short run, which is why it is a fraction
of a percent behind v2 here rather than ahead.)
