#!/usr/bin/env bash
# suite.sh -- the complete evaluation.
#
# Three kinds of evidence:
#   1. Instruction counts (callgrind) on micro workloads -- exact, no noise.
#   2. Cache and branch behaviour (cachegrind) -- shows *why* a design wins.
#   3. Wall clock and peak RSS on real programs (SPEC CPU2006 derived:
#      cfrac, espresso, barnes) and on a multithreaded workload -- the
#      numbers that decide "fastest in most use cases".
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

section() { printf "\n\033[1m%s\033[0m\n" "$*"; }

# ---------------------------------------------------------------- 1
section "1. Instruction counts (callgrind) -- exact, noise-free"
printf "%-24s %11s %11s %11s %11s %11s %11s\n" workload "${NAMES[@]}"
for wl in "churn 300000 64" "churn 300000 8" "churn 200000 1024" \
          "churnmix 200000" "mixed 150000" "retain 150000" "cross 150000"; do
  set -- $wl
  mode=$1; shift
  args="$*"
  printf "%-24s" "$mode $args"
  for n in "${NAMES[@]}"; do
    lib=${ALLOC[$n]}
    out=/tmp/opencode/s_$$.out
    if [ -z "$lib" ]; then
      valgrind --tool=callgrind --callgrind-out-file=$out --quiet \
        ./bench/instr $mode $args >/dev/null 2>&1
    else
      env LD_PRELOAD="$lib" valgrind --tool=callgrind --callgrind-out-file=$out \
        --quiet ./bench/instr $mode $args >/dev/null 2>&1
    fi
    ir=$(grep '^summary:' $out | awk '{print $2}')
    printf " %11s" "$ir"
  done
  echo
done
rm -f /tmp/opencode/s_$$.out

# ---------------------------------------------------------------- 2
section "2. Cache and branch behaviour (cachegrind)"
for wl in "churn 300000 64" "churnmix 200000"; do
  set -- $wl; mode=$1; shift; args="$*"
  printf "\n%s %s\n" "$mode" "$args"
  printf "%-11s %12s %10s %10s %9s %12s\n" allocator Ir D1_miss LL_miss D1mr% Bcm%
  for n in "${NAMES[@]}"; do
    lib=${ALLOC[$n]}
    out=/tmp/opencode/c_$$.out
    if [ -z "$lib" ]; then
      valgrind --tool=cachegrind --cache-sim=yes --branch-sim=yes \
        --cachegrind-out-file=$out --quiet ./bench/instr $mode $args >/dev/null 2>&1
    else
      env LD_PRELOAD="$lib" valgrind --tool=cachegrind --cache-sim=yes \
        --branch-sim=yes --cachegrind-out-file=$out --quiet \
        ./bench/instr $mode $args >/dev/null 2>&1
    fi
    read ir d1 ll d1p bc bcm < <(python3 bench/cg_annotate.py $out)
    printf "%-11s %12s %10s %10s %9s %12s\n" "$n" "$ir" "$d1" "$ll" "$d1p" "$bcm"
  done
  rm -f /tmp/opencode/c_$$.out
done

# ---------------------------------------------------------------- 3
section "3. Real programs -- wall clock (min of $REPS) and peak RSS"
run_real() { # run_real <label> <command string with %LIB%>
  local label=$1 cmd=$2
  printf "\n%s\n" "$label"
  printf "%-11s %10s %10s\n" allocator sec rss_kb
  for n in "${NAMES[@]}"; do
    local lib=${ALLOC[$n]} best="" bestrss=""
    for ((r=0;r<REPS;r++)); do
      local out
      if [ -z "$lib" ]; then
        out=$( { /usr/bin/time -f "%e %M" bash -c "$cmd" >/dev/null; } 2>&1 | tail -1 )
      else
        out=$( { /usr/bin/time -f "%e %M" bash -c "LD_PRELOAD=$lib $cmd" >/dev/null; } 2>&1 | tail -1 )
      fi
      [ -z "$out" ] && continue
      local sec=${out% *} rss=${out#* }
      if [ -z "$best" ]; then best=$sec; bestrss=$rss; else
        best=$(echo "$sec $best" | awk '{print ($1<$2)?$1:$2}')
        bestrss=$rss
      fi
    done
    printf "%-11s %10s %10s\n" "$n" "$best" "$bestrss"
  done
}

run_real "cfrac (factoring, 44-digit)" \
  "$RW/cfrac 17545186520507317056371138836327483792789528"
run_real "espresso (boolean minimisation)" \
  "$RW/espresso $BENCH/espresso/largest.espresso"
run_real "barnes (n-body, 164k bodies, 123 steps)" \
  "$RW/barnes < $BENCH/barnes/input"

section "4. Multi-threaded (server-style)"
printf "%-11s %12s %12s\n" allocator "local s" "xfree s"
for n in "${NAMES[@]}"; do
  lib=${ALLOC[$n]}
  bl=999; bx=999
  for ((r=0;r<REPS;r++)); do
    for mode in 0 1; do
      start=$(date +%s.%N)
      if [ -z "$lib" ]; then
        taskset -c 0-7 ./bench/mt $mode 2000000 8 >/dev/null 2>&1
      else
        taskset -c 0-7 env LD_PRELOAD="$lib" ./bench/mt $mode 2000000 8 >/dev/null 2>&1
      fi
      end=$(date +%s.%N)
      t=$(echo "$start $end" | awk '{printf "%.3f", $2-$1}')
      if [ "$mode" = "0" ]; then bl=$(echo "$t $bl"|awk '{print ($1<$2)?$1:$2}');
      else bx=$(echo "$t $bx"|awk '{print ($1<$2)?$1:$2}'); fi
    done
  done
  printf "%-11s %12s %12s\n" "$n" "$bl" "$bx"
done
