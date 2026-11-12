#!/bin/bash
# Macro (realistic workload) benchmark runner.
# Interleaved rounds, min-of-REPS per allocator, same allocator set as
# run.sh plus v2. Unlike run.sh this script refuses to fall back silently:
# a missing library is reported, not replaced by glibc numbers.
# Usage: REPS=3 ./bench/macro.sh
set -u
cd "$(dirname "$0")/.."
REPS=${REPS:-3}

declare -A LIBS=( [glibc]="" [v1]="./libfastmalloc.so" [v2]="./libfastmalloc2.so" [jit]="./libjitmalloc.so" )
ORDER=(glibc v1 v2 jit)

declare -A BEST BESTRSS
run_one() {
    local label=$1; shift
    local lib="${LIBS[$label]}"
    if [ -n "$lib" ] && [ ! -f "$lib" ]; then
        echo "  skip $label ($lib missing)" >&2
        return
    fi
    local out
    if [ -n "$lib" ]; then
        out=$(LD_PRELOAD="$PWD/$lib" timeout 300 ./bench/macro "$@" 2>/dev/null)
    else
        out=$(timeout 300 ./bench/macro "$@" 2>/dev/null)
    fi
    local ns rss
    ns=$(echo "$out" | sed -n 's/.*"ns_per_op":\([0-9.]*\).*/\1/p')
    rss=$(echo "$out" | sed -n 's/.*"rss_kb":\([0-9]*\).*/\1/p')
    if [ -z "$ns" ]; then
        echo "  FAIL $label $* " >&2
        return
    fi
    local key="$1:$label"
    if [ -z "${BEST[$key]:-}" ] || awk "BEGIN{exit !($ns < ${BEST[$key]})}"; then
        BEST[$key]=$ns
        BESTRSS[$key]=$rss
    fi
}

bench() {
    local name=$1; shift
    for r in $(seq 1 $REPS); do
        for a in "${ORDER[@]}"; do run_one "$a" "$name" "$@"; done
    done
    for a in "${ORDER[@]}"; do
        printf '%-10s %-6s %12s %10s\n' "$name" "$a" "${BEST[$name:$a]:--}" "${BESTRSS[$name:$a]:--}"
    done
}

printf '%-10s %-6s %12s %10s\n' scenario alloc ns/op rss_kb
bench server   4 400000
bench compiler 600
bench jsondoc  6000
bench kvstore  2000000
bench game     15000
bench locality 10

# trim: the interesting output is the RSS trajectory, not ns/op, so the
# raw JSON lines are printed (one run per allocator is enough -- the
# numbers are page counts, not timings).
for a in "${ORDER[@]}"; do
    lib="${LIBS[$a]}"
    if [ -n "$lib" ] && [ ! -f "$lib" ]; then continue; fi
    if [ -n "$lib" ]; then
        out=$(LD_PRELOAD="$PWD/$lib" timeout 300 ./bench/macro trim 1 2>/dev/null)
    else
        out=$(timeout 300 ./bench/macro trim 1 2>/dev/null)
    fi
    printf 'trim       %-6s %s\n' "$a" "$out"
done
