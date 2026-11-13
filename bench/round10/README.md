# Round 10 ownership-lock experiment

From the repository root:

- `bash bench/round10/lock-cost.sh`: build an exact f90550c control, the rejected
  compile-time central-stripe patch and the adaptive census candidate; run the
  unchanged round-8 ownership workloads, alternating baseline/candidate order.
- `bash bench/round10/lock-cost-verify.sh`: full make/round7 batteries plus three
  FM_DEBUG stress repetitions per implementation/configuration and the focused
  busy-census check.

The timing driver uses one excluded warmup pair and five measured pairs per
case, wall time around the subprocess, medians, full ranges, CVs and paired
counts. It asserts harness success. No best-of-N reducer is used. It overwrites
its result paths, so copy existing evidence before rerunning. Timing and
correctness must run sequentially. See ADRs 0047–0048 for ownership reasoning,
rejected alternatives, measured evidence and reclamation-latency tradeoffs.

The stripe patch targets f90550c, not the adopted adaptive source. It uses the
Linux/glibc zero-valued normal-mutex representation and is a rejected Linux
prototype, not a portable synchronization library. Combining it with the epoch
prototype deliberately fails compilation. It is never built by make.

The adopted build defaults `FM_CENSUS_TRYLOCK` to 1; `=0` selects blocking
censuses. In the original validation artifacts, `default-*` names the
pre-promotion blocking build and `adaptive-*` the enabled prototype.
`promoted-*` records the ordinary adopted default. The verifier now labels
its configurations `blocking` and `default` to reflect the adopted source.
Use `RESULTS_DIR=/path/to/new/results` to preserve the original validation logs.

Build transcript trailing whitespace was trimmed; sample data is unchanged.
