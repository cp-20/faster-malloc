#!/usr/bin/env bash
# noise.sh -- show the distribution of repeated runs for one allocator so we
# can judge how noisy the machine is and how many reps we need.
set -u
cd "$(dirname "$0")/.."
BIN=$PWD/bench/micro
LIB=${1:-}
N=${2:-12}
shift 2 2>/dev/null || true
for i in $(seq 1 $N); do
  if [ -z "$LIB" ]; then
    taskset -c "${CPU:-3}" "$BIN" "$@" 2>/dev/null | sed -n 's/.*"ns_per_op":\([0-9.]*\).*/\1/p'
  else
    taskset -c "${CPU:-3}" env LD_PRELOAD="$LIB" "$BIN" "$@" 2>/dev/null | sed -n 's/.*"ns_per_op":\([0-9.]*\).*/\1/p'
  fi
done | sort -g | awk '{a[NR]=$1} END{printf "min=%.2f  p25=%.2f  med=%.2f  p75=%.2f  max=%.2f  (n=%d)\n", a[1], a[int(NR*0.25)+1], a[int((NR+1)/2)], a[int(NR*0.75)+1], a[NR], NR}'