# Reclamation experiment (ADR 0041)

From the repository root:

```sh
bash bench/round9/measure.sh
bash bench/round9/verify.sh
```

The first command builds the exact d9c4364 v2 control from git objects and
an O3 `FM_RECLAIM_EPOCH` prototype. It checks that the normal v2 `.text`
still equals that control. `measure.py` runs one alternating warmup pair
and five alternating measured pairs for local, cross-free, ownership,
ownership+trim, and the existing bursty app workload. It preserves every
successful sample (including warmups) and stops on an assertion/exit failure,
recording the child's status and output in `failure.json`. Remove old result
files before an independent rerun; these scripts replace their named outputs.
The reported medians and ranges exclude warmups; wall time includes process
startup and exit. The bursty checksum must match across all samples. Its
RSS/phase output is retained; sleep time makes its wall time insensitive.

The unchanged ownership harness has four producer/consumer pairs and 128
requested sizes mapping to 64 v2 classes, 200,000 transfers per pair,
endpoint-byte assertions, and final transfer counts. Local/cross-free use
10,000,000 operations (`bench/mt` cross-free always has two threads).

The second command runs the full round7 battery for **both** the locked and
epoch configurations, including both implementations and the idlelifecycle
fault matrix. Its CC wrapper propagates the prototype define into custom
lifecycle DSOs. It also runs three FM_DEBUG stress repetitions for each
implementation/configuration (12 total), with age zero, minimum one purge
page, idle release and realloc cache search enabled. A trap rebuilds defaults
on exit. `grace.sh`, also part of round7.sh, deterministically pins a reader
and asserts header reuse cannot cross the grace period. This is a structural
regression check, not a throughput test or an exhaustive race detector.

`bench/results/round9-reclamation/preliminary/` retains the discarded first
measurement series: its locked control used equivalent function wrappers
rather than original macro-expanded lock calls, and had different machine
code. A bursty control sample exited with SIGSEGV; the initial script did not
retain that failed child's stdout. The traceback is retained. Standalone
original/prototype bursty reruns both passed (phase outputs retained). The
final series uses untouched git-object control code; no sample from the
preliminary series is mixed into final medians. The only reclamation
algorithm attempted is the synchronous global-epoch prototype.
