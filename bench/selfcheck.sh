#!/usr/bin/env bash
# selfcheck.sh -- measure the harness's own error floor by benchmarking the
# SAME allocator under two different labels.  Any difference is pure noise.
set -u
cd "$(dirname "$0")/.."
BIN=$PWD/bench/micro
SC=${1:-churn}
shift || true
ARGS="$*"
REPS=${REPS:-15}
declare -A L
L[A]="$PWD/libfastmalloc.so"
L[B]="$PWD/libfastmalloc.so"
L[C]="libmimalloc.so.2"
L[D]="libmimalloc.so.2"
declare -A best
for k in A B C D; do best[$k]=999999; done
for ((r=0;r<REPS;r++)); do
  for k in A B C D; do
    v=$(taskset -c "${CPU:-3}" env LD_PRELOAD="${L[$k]}" "$BIN" "$SC" $ARGS 2>/dev/null | sed -n 's/.*"ns_per_op":\([0-9.]*\).*/\1/p')
    [ -z "$v" ] && continue
    best[$k]=$(echo "$v ${best[$k]}" | awk '{print ($1<$2)?$1:$2}')
  done
done
for k in A B C D; do printf "%s=%s  " "$k" "${best[$k]}"; done
echo
echo "A vs B (same allocator): $(echo "${best[A]} ${best[B]}" | awk '{printf "%.1f%%", ($1-$2)/$1*100}')"
echo "C vs D (same allocator): $(echo "${best[C]} ${best[D]}" | awk '{printf "%.1f%%", ($1-$2)/$1*100}')"
echo "A vs C (different):      $(echo "${best[A]} ${best[C]}" | awk '{printf "%.1f%%", ($1-$2)/$1*100}')"