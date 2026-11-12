#!/usr/bin/env bash
# jitcheck.sh -- isolate the JIT's steady-state effect.
#
# The JIT build and the static build differ by more than the code generator:
# the JIT build routes every call through a trampoline and pays a sampling
# branch.  To measure what the *code generation* buys, compare the same
# binary with FM_JIT=0 (generic path, same trampoline) against FM_JIT=1.
set -u
ROOT=/home/cp20/playground/faster-malloc
cd "$ROOT"
BIN=$ROOT/bench/instr
MODE=${1:-churn}
N=${2:-400000}
LIB=${3:-$ROOT/libjitmalloc.so}

count() { # count <env-assignments...>
  local out=/tmp/opencode/jc_$$.out
  env "$@" LD_PRELOAD="$LIB" valgrind --tool=callgrind \
      --callgrind-out-file="$out" --quiet "$BIN" "$MODE" "$N" >/dev/null 2>&1
  grep '^summary:' "$out" | awk '{print $2}'
}

base=$(count FM_JIT=0 FM_JIT_PROFILE=0 X=1 2>/dev/null)
base=$(LD_PRELOAD="$LIB" valgrind --tool=callgrind \
   --callgrind-out-file=/tmp/opencode/jb_$$.out --quiet "$BIN" "$MODE" 0 \
   >/dev/null 2>&1; grep '^summary:' /tmp/opencode/jb_$$.out | awk '{print $2}')

for mode in "FM_JIT=0" "FM_JIT=1" "FM_JIT=1 FM_JIT_PROFILE=0"; do
  tot=$(count $mode)
  d=$(echo "$tot $base" | awk '{print $1-$2}')
  printf "%-28s total=%-12s alloc=%-12s Ir/alloc=%.2f\n" "$mode" "$tot" "$d" \
     "$(echo "$d $N" | awk '{printf "%.2f", $1/$2}')"
done
rm -f /tmp/opencode/jc_$$.out /tmp/opencode/jb_$$.out
