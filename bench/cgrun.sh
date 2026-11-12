#!/usr/bin/env bash
# cgrun.sh -- exact instruction-count comparison of allocators using callgrind.
#
# Wall-clock on this host has a ~3.5% noise floor with 5x outliers, so
# instruction counts (exact, reproducible) are the primary evidence for
# "which allocator does less work per allocation".  Wall-clock is reported
# separately on real workloads where the effect is large enough to see.
#
# usage: bench/cgrun.sh <mode> [iters]
set -u
ROOT=/home/cp20/playground/faster-malloc
BIN=$ROOT/bench/instr
MODE=${1:-churn}
ITERS=${2:-100000}
CG=${CG:-1}

declare -A ALLOC
ALLOC[glibc]=""
ALLOC[tcmalloc]="libtcmalloc_minimal.so.4"
ALLOC[jemalloc]="libjemalloc.so.2"
ALLOC[mimalloc]="libmimalloc.so.2"
ALLOC[v1]="$ROOT/libfastmalloc.so"
ALLOC[v2]="$ROOT/libfastmalloc2.so"
ALLOC[jit]="$ROOT/libjitmalloc.so"

NAMES=(glibc tcmalloc jemalloc mimalloc v1 v2 jit)

# Number of instructions the *allocator* itself executes, i.e. the cost we
# actually control.  Startup (ld.so) is excluded by measuring the delta
# between two iteration counts.
measure() {
  local name=$1 n=$2
  local lib=${ALLOC[$name]}
  local out=/tmp/opencode/cg_${name}_${n}.out
  if [ -z "$lib" ]; then
    valgrind --tool=callgrind --callgrind-out-file=$out --quiet \
      $BIN $MODE $n >/dev/null 2>&1
  else
    LD_PRELOAD=$lib valgrind --tool=callgrind --callgrind-out-file=$out --quiet \
      $BIN $MODE $n >/dev/null 2>&1
  fi
  callgrind_annotate --threshold=100 $out 2>/dev/null | \
    awk '/PROGRAM TOTALS/ {gsub(/,/,"",$1); print $1}'
}

if [ "$CG" = "1" ]; then
  N1=20000; N2=$ITERS
  printf "%-12s %14s %14s %12s\n" allocator "total_Ir" "alloc_Ir" "Ir/alloc"
  for n in "${NAMES[@]}"; do
    a=$(measure "$n" $N1); b=$(measure "$n" $N2)
    d=$(echo "$b $a" | awk '{print $1-$2}')
    printf "%-12s %14s %14s %12s\n" "$n" "$b" "$d" \
      "$(echo "$d $((N2-N1))" | awk '{printf "%.1f", $1/$2}')"
  done
else
  REPS=${REPS:-9}
  printf "%-12s %12s\n" allocator "ns/op"
  for n in "${NAMES[@]}"; do
    lib=${ALLOC[$n]}
    best=""
    for ((r=0;r<REPS;r++)); do
      if [ -z "$lib" ]; then
        v=$(taskset -c 3 $BIN $MODE $ITERS 2>/dev/null | tail -1)
      else
        v=$(taskset -c 3 env LD_PRELOAD=$lib $BIN $MODE $ITERS 2>/dev/null | tail -1)
      fi
      [ -z "$v" ] && continue
    done
    :
  done
fi
