#!/usr/bin/env bash
# sweep.sh -- measure the effect of the per-class local-cache limit.
#
# The limit controls how many blocks a thread may keep in its private free
# list before handing them to the shared central list.  It trades memory
# retained per thread against how often the flush branch is taken.
set -u
ROOT=/home/cp20/playground/faster-malloc
cd "$ROOT"
MODE=${1:-churn}
N=${2:-300000}
CAPS=${CAPS:-"16 32 64 128 256 512"}

declare -A ALLOC
ALLOC[glibc]=""
ALLOC[tcmalloc]="libtcmalloc_minimal.so.4"
ALLOC[jemalloc]="libjemalloc.so.2"
ALLOC[mimalloc]="libmimalloc.so.2"
ALLOC[v1]="$ROOT/libfastmalloc.so"
ALLOC[v2]="$ROOT/libfastmalloc2.so"
ALLOC[jit]="$ROOT/libjitmalloc.so"
NAMES=(glibc tcmalloc jemalloc mimalloc v1 v2 jit)

ir_of() { # ir_of <lib> <cmd...>
  local lib=$1; shift
  local out=/tmp/opencode/sw_$$.out
  if [ -z "$lib" ]; then
    valgrind --tool=callgrind --cache-sim=yes --branch-sim=yes \
      --cachegrind-out-file="$out" --quiet "$@" >/dev/null 2>&1
  else
    env LD_PRELOAD="$lib" valgrind --tool=cachegrind --cache-sim=yes \
      --branch-sim=yes --cachegrind-out-file="$out" --quiet "$@" >/dev/null 2>&1
  fi
  python3 "$ROOT/bench/cg_annotate.py" "$out"
}

echo "=== static allocators, mode=$MODE ==="
printf "%-11s %12s %10s %10s %10s\n" allocator Ir D1miss LLmiss Bcm%
for name in "${NAMES[@]}"; do
  lib=${ALLOC[$name]}
  if [ "$name" = "v2" ] || [ "$name" = "jit" ]; then continue; fi
  read ir d1 ll d1p bc bcm < <(ir_of "$lib" ./bench/instr "$MODE" "$N")
  printf "%-11s %12s %10s %10s %10s\n" "$name" "$ir" "$d1" "$ll" "$bcm"
done

echo "=== v2 with different local-cache limits ==="
printf "%-11s %12s %10s %10s %10s\n" limit Ir D1miss LLmiss Bcm%
for cap in $CAPS; do
  FM_LIMIT_CAP=$cap python3 src/gen_classes2.py src/size_classes2.h 2>/dev/null
  make libfastmalloc2.so >/dev/null 2>&1
  read ir d1 ll d1p bc bcm < <(ir_of "$ROOT/libfastmalloc2.so" ./bench/instr "$MODE" "$N")
  printf "%-11s %12s %10s %10s %10s\n" "cap=$cap" "$ir" "$d1" "$ll" "$bcm"
done
rm -f /tmp/opencode/sw_$$.out
