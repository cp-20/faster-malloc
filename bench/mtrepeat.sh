#!/usr/bin/env bash
# mtrepeat.sh -- repeat the 8-thread micro until a hang/crash shows up.
# Guards ADR 0076: the old lock-free batch-header freelist was ABA-prone
# and hung or segfaulted in ~1 of 4 runs of `micro threads`.
set -u
cd "$(dirname "$0")/.."
N=${N:-30}
fail=0
for lib in "$PWD/libfastmalloc2.so" "$PWD/libjitmalloc.so"; do
  for ((i=0;i<N;i++)); do
    LD_PRELOAD=$lib timeout 10 bench/micro threads 8 64 5000000 >/dev/null 2>&1 \
      || { echo "FAIL ${lib##*/} run $i rc=$?"; fail=1; }
    LD_PRELOAD=$lib timeout 20 bench/micro cross 64 4096 256 >/dev/null 2>&1 \
      || { echo "FAIL ${lib##*/} cross run $i rc=$?"; fail=1; }
  done
done
[ $fail = 0 ] && echo "mtrepeat ok ($N runs x 2 libs)"
exit $fail
