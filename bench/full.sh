#!/usr/bin/env bash
# full.sh -- the headline comparison.
#
# For each workload and allocator it reports:
#   Ir    exact instruction count (callgrind) -- deterministic, no noise
#   sec   wall clock, min of REPS interleaved runs
#   rss   peak resident set size in KiB
#
# usage: bench/full.sh [workload ...]
set -u
ROOT=/home/cp20/playground/faster-malloc
cd "$ROOT"
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

# workload -> "label|cmd"
workloads=()
add_wl() { workloads+=("$1|$2"); }

add_wl "churn"    "$ROOT/bench/instr churn 400000"
add_wl "churnmix" "$ROOT/bench/instr churnmix 200000"
add_wl "mixed"    "$ROOT/bench/instr mixed 150000"
add_wl "retain"   "$ROOT/bench/instr retain 150000"
add_wl "cross"    "$ROOT/bench/instr cross 150000"
add_wl "cfrac-s"  "$RW/cfrac 12345678901234567890123"
add_wl "cfrac-m"  "$RW/cfrac 856070387728264"
add_wl "cfrac-l"  "$RW/cfrac 17545186520507317056371138836327483792789528"
add_wl "espresso" "$RW/espresso $BENCH/espresso/largest.espresso"

if [ $# -gt 0 ]; then
  sel=()
  for w in "$@"; do for x in "${workloads[@]}"; do
    case "$x" in "$w"*) sel+=("$x");; esac
  done; done
  workloads=("${sel[@]}")
fi

ir_count() { # ir_count <name> <cmd...>
  local name=$1; shift
  local lib=${ALLOC[$name]}
  local out=/tmp/opencode/fc_$$.out
  if [ -z "$lib" ]; then
    taskset -c "$CPU" valgrind --tool=callgrind --callgrind-out-file="$out" \
      --quiet "$@" >/dev/null 2>&1
  else
    taskset -c "$CPU" env LD_PRELOAD="$lib" valgrind --tool=callgrind \
      --callgrind-out-file="$out" --quiet "$@" >/dev/null 2>&1
  fi
  grep '^summary:' "$out" | awk '{print $2}'
}

time_and_rss() { # time_and_rss <name> <cmd...> -> "sec rss"
  local name=$1; shift
  local lib=${ALLOC[$name]}
  local start end rss
  start=$(date +%s.%N)
  if [ -z "$lib" ]; then
    rss=$( { /usr/bin/time -f "%M" taskset -c "$CPU" "$@" >/dev/null; } 2>&1 | tail -1 )
  else
    rss=$( { /usr/bin/time -f "%M" taskset -c "$CPU" env LD_PRELOAD="$lib" "$@" >/dev/null; } 2>&1 | tail -1 )
  fi
  end=$(date +%s.%N)
  echo "$start $end $rss" | awk '{printf "%.2f %s", $2-$1, $3}'
}

printf "%-10s %-11s %12s %8s %10s\n" workload allocator Ir sec rss_kb
printf -- "---------------------------------------------------------------------\n"
for w in "${workloads[@]}"; do
  label=${w%%|*}; cmd=${w#*|}
  base=""
  for name in "${NAMES[@]}"; do
    ir=$(ir_count "$name" $cmd)
    tr=$(time_and_rss "$name" $cmd)
    sec=${tr%% *}; rss=${tr##* }
    if [ -z "$base" ]; then base=$ir; fi
    delta=$(echo "$ir $base" | awk '{printf "%+.1f%%", ($1-$2)/$2*100}')
    printf "%-10s %-11s %12s %8s %10s\n" "$label" "$name" "$ir" "$sec" "$rss"
  done
  echo
done
