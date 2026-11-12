#!/usr/bin/env bash
# rw.sh -- real-world allocator benchmarks.
#
# These are whole programs (SPEC CPU2006 derived: cfrac, espresso, barnes)
# plus a multi-threaded server-style workload.  Unlike the microbenchmarks
# these have low run-to-run variance, so they are the primary evidence for
# "fastest in most use cases".
#
# Method: run each allocator REPS times, interleaved (round robin) so that
# machine drift affects all of them equally, and report the minimum.
set -u
ROOT=/home/cp20/playground/faster-malloc
RW=${RW:-/tmp/opencode/rw}
BENCH=${BENCH:-/tmp/opencode/mimalloc-bench/bench}
REPS=${REPS:-3}
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

time_cmd() { # time_cmd <allocator> <cmd...>  -> seconds on stdout
  local name=$1; shift
  local lib=${ALLOC[$name]}
  if [ -z "$lib" ]; then
    taskset -c "$CPU" "$@" 2>&1 >/dev/null | tail -1
  else
    taskset -c "$CPU" env LD_PRELOAD="$lib" "$@" 2>&1 >/dev/null | tail -1
  fi
}

run_timed() { # run_timed <allocator> <cmd...> -> elapsed seconds
  local name=$1; shift
  local lib=${ALLOC[$name]}
  local start end
  start=$(date +%s.%N)
  if [ -z "$lib" ]; then
    taskset -c "$CPU" "$@" >/dev/null 2>&1
  else
    taskset -c "$CPU" env LD_PRELOAD="$lib" "$@" >/dev/null 2>&1
  fi
  end=$(date +%s.%N)
  echo "$start $end" | awk '{printf "%.3f", $2-$1}'
}

best_of() { # best_of <allocator> <cmd...>
  local name=$1; shift
  local best=""
  for ((r=0;r<REPS;r++)); do
    local t=$(run_timed "$name" "$@")
    [ -z "$t" ] && continue
    if [ -z "$best" ]; then best=$t; else
      best=$(echo "$t $best" | awk '{print ($1<$2)?$1:$2}')
    fi
  done
  echo "$best"
}

printf "%-12s" "workload"
for n in "${NAMES[@]}"; do printf " %10s" "$n"; done
echo

report() { # report <label> <cmd...>
  local label=$1; shift
  printf "%-12s" "$label"
  local base=""
  for n in "${NAMES[@]}"; do
    local t=$(best_of "$n" "$@")
    if [ -z "$base" ]; then base=$t; fi
    printf " %10s" "$t"
  done
  echo
}

# cfrac with three input sizes (small / medium / large)
report "cfrac-s"  "$RW/cfrac" 12345678901234567890123
report "cfrac-m"  "$RW/cfrac" 856070387728264
report "cfrac-l"  "$RW/cfrac" 17545186520507317056371138836327483792789528

# espresso: boolean minimisation, very allocation heavy
report "espresso" "$RW/espresso" "$BENCH/espresso/largest.espresso"

# barnes: n-body, long-running numeric
report "barnes"   "$RW/barnes"
