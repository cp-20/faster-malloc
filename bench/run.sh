#!/usr/bin/env bash
# run.sh -- compare allocators across micro scenarios.
#
# Methodology (this box is a shared, heavily virtualized WSL2 host, so
# absolute timings are inflated and drift over time):
#   * allocators are run round-robin (interleaved) so any drift hits them all
#   * we report the MIN over repetitions (min = least-interfered sample)
#   * workloads are long enough that per-run overhead is negligible
#
# usage: bench/run.sh [scenario ...]
set -u
cd "$(dirname "$0")/.."
ROOT=$PWD
BIN=$ROOT/bench/micro
REPS=${REPS:-9}
CPU=${CPU:-3}

declare -A ALLOC
ALLOC[glibc]=""
ALLOC[tcmalloc]="libtcmalloc_minimal.so.4"
ALLOC[jemalloc]="libjemalloc.so.2"
ALLOC[mimalloc]="libmimalloc.so.2"
ALLOC[fastmalloc]="$ROOT/libfastmalloc.so"
ALLOC[jitmalloc]="$ROOT/libjitmalloc.so"

# scenario -> "subcommand args..."
declare -A CMD
CMD[churn8]="churn 8 20000000"
CMD[churn]="churn 64 20000000"
CMD[churn1024]="churn 1024 10000000"
CMD[batch]="batch 64 100000 20"
CMD[batch1024]="batch 1024 50000 20"
CMD[rand]="rand 16 1024 100000 20"
CMD[pareto]="pareto 100000 20"
CMD[hold]="hold 64 100000 20"
CMD[threads]="threads 8 64 5000000"
CMD[cross]="cross 64 65536 1"
CMD[large]="large 65536 200000"
CMD[realloc]="realloc 64 5000000"
CMD[calloc]="calloc 1024 64 20000"
CMD[touch]="touch 4096 200000"

if [ $# -gt 0 ]; then
  SCENARIOS=("$@")
else
  SCENARIOS=(churn8 churn churn1024 batch batch1024 rand pareto hold threads cross large realloc calloc touch)
fi

NAMES=(glibc tcmalloc jemalloc mimalloc fastmalloc jitmalloc)

run_one() {
  local name=$1 cmd=$2
  local lib=${ALLOC[$name]}
  if [ -z "$lib" ]; then
    taskset -c "$CPU" "$BIN" $cmd 2>/dev/null
  else
    taskset -c "$CPU" env LD_PRELOAD="$lib" "$BIN" $cmd 2>/dev/null
  fi
}

printf "%-11s %-11s %12s %14s %10s\n" scenario alloc ns/op ops/s rss_kb
printf "%s\n" "-------------------------------------------------------------------"
for sc in "${SCENARIOS[@]}"; do
  [ -z "${CMD[$sc]}" ] && continue
  declare -A best
  for name in "${NAMES[@]}"; do best[$name]=999999; done
  out=""
  for ((r=0;r<REPS;r++)); do
    for name in "${NAMES[@]}"; do
      out=$(run_one "$name" "${CMD[$sc]}")
      v=$(echo "$out" | sed -n 's/.*"ns_per_op":\([0-9.]*\).*/\1/p')
      [ -z "$v" ] && continue
      cur=${best[$name]}
      best[$name]=$(echo "$v $cur" | awk '{print ($1<$2)?$1:$2}')
    done
  done
  for name in "${NAMES[@]}"; do
    rss=$(echo "$out" | sed -n 's/.*"rss_kb":\([0-9]*\).*/\1/p')
    printf "%-11s %-11s %12s %14s %10s\n" "$sc" "$name" "${best[$name]}" \
      "$(echo "${best[$name]}" | awk '{printf "%.0f", 1e9/$1}')" "$rss"
  done
  echo
done
