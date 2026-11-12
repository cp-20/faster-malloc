#!/usr/bin/env bash
# cache.sh -- cachegrind comparison: instruction counts *and* data cache
# behaviour.  Instruction counts alone miss the locality effects that decide
# real-world allocator performance (objects freed together should be reused
# together so they stay in cache).
set -u
ROOT=/home/cp20/playground/faster-malloc
cd "$ROOT"
RW=${RW:-/tmp/opencode/rw}
BENCH=${BENCH:-/tmp/opencode/mimalloc-bench/bench}
CPU=${CPU:-3}

declare -A ALLOC
ALLOC[glibc]=""
ALLOC[tcmalloc]="libtcmalloc_minimal.so.4"
ALLOC[jemalloc]="libjemalloc.so.2"
ALLOC[mimalloc]="libmimalloc.so.2"
ALLOC[v1]="$ROOT/libfastmalloc.so"
ALLOC[v2]="$ROOT/libfastmalloc2.so"
ALLOC[jit]="$ROOT/libjitmalloc.so"
NAMES=(glibc tcmalloc jemalloc mimalloc v1 v2 jit)

wl=${1:-espresso}
case "$wl" in
  churn)    cmd="$ROOT/bench/instr churn 300000";;
  churnmix) cmd="$ROOT/bench/instr churnmix 200000";;
  mixed)    cmd="$ROOT/bench/instr mixed 150000";;
  cfrac-m)  cmd="$RW/cfrac 856070387728264";;
  cfrac-l)  cmd="$RW/cfrac 17545186520507317056371138836327483792789528";;
  espresso) cmd="$RW/espresso $BENCH/espresso/largest.espresso";;
esac

printf "%-11s %12s %10s %10s %8s %12s %10s\n" allocator Ir D1_miss LL_miss D1mr% Branches Bcm%
for name in "${NAMES[@]}"; do
  lib=${ALLOC[$name]}
  out=/tmp/opencode/cg_c_$$.out
  if [ -z "$lib" ]; then
    taskset -c "$CPU" valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes --cachegrind-out-file="$out" \
      --quiet $cmd >/dev/null 2>&1
  else
    taskset -c "$CPU" env LD_PRELOAD="$lib" valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes \
      --cachegrind-out-file="$out" --quiet $cmd >/dev/null 2>&1
  fi
  read ir d1 ll d1p bc bcm bcmp < <(python3 "$ROOT/bench/cg_annotate.py" "$out")
  printf "%-11s %12s %10s %10s %8s %12s %10s\n" "$name" "$ir" "$d1" "$ll" "$d1p" "$bc" "$bcm"
done
rm -f /tmp/opencode/cg_c_$$.out
