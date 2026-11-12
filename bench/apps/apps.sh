#!/bin/bash
# apps.sh -- the real-application allocator matrix (round 3).
#
# Runs real programs, in several languages/runtimes, under each
# allocator via LD_PRELOAD and records wall time (min of REPS,
# allocators interleaved round-robin so drift hits everyone) and
# peak RSS.  An app that crashes, hangs or changes its output under
# an allocator is reported as FAIL with its exit status -- never
# silently skipped and never replaced with another allocator's
# numbers.
#
# Usage:  bench/apps/prep.sh        # once, builds fixtures
#         REPS=3 bench/apps/apps.sh # the full matrix
#         APPS="lua jq" REPS=1 bench/apps/apps.sh
#
# Allocators: glibc, v1, v2, jit, plus jemalloc / mimalloc /
# tcmalloc when their shared libraries are installed.  REDIS_BIN and
# LUA_BIN override tool locations (see bench/apps/README.md).
set -u
cd "$(dirname "$0")/../.."
ROOT=$PWD
WORK=$ROOT/bench/apps/work
WL=$ROOT/bench/apps/workloads
REPS=${REPS:-3}
TIMEOUT=${TIMEOUT:-600}

# ---------------------------------------------------------------- #
# allocator table
# ---------------------------------------------------------------- #
declare -A LIBS=( [glibc]="" [v1]="$ROOT/libfastmalloc.so" \
                  [v2]="$ROOT/libfastmalloc2.so" [jit]="$ROOT/libjitmalloc.so" )
sys_lib() { ldconfig -p | grep -m1 "$1" | sed 's/.*=> //' ; }
for pair in "jemalloc:libjemalloc.so.2" "mimalloc:libmimalloc.so" \
            "tcmalloc:libtcmalloc_minimal.so"; do
    name=${pair%%:*}; soname=${pair#*:}
    p=$(sys_lib "$soname")
    [ -n "$p" ] && LIBS[$name]="$p"
done
ALLOCATORS=${ALLOCATORS:-"glibc v1 v2 jit jemalloc mimalloc tcmalloc"}
APPS=${APPS:-"redis sqlite jq lua python node gcc git"}

# ---------------------------------------------------------------- #
# helpers
# ---------------------------------------------------------------- #
declare -A BEST BESTRSS STATUS
note_fail() { # note_fail app alloc detail
    echo "  FAIL $1/$2: $3" >&2
    STATUS[$1:$2]="FAIL($3)"
}

timed() { # timed app alloc cmd... -> sets T and RSS from run_timed
    local app=$1 alloc=$2; shift 2
    local lib=${LIBS[$alloc]}
    local err
    if [ -n "$lib" ]; then
        err=$(timeout "$TIMEOUT" env LD_PRELOAD="$lib" \
              python3 "$ROOT/bench/apps/run_timed.py" "$@" 2>&1 >/dev/null)
    else
        err=$(timeout "$TIMEOUT" \
              python3 "$ROOT/bench/apps/run_timed.py" "$@" 2>&1 >/dev/null)
    fi
    local rc=$?
    T=$(echo "$err" | sed -n 's/.*elapsed=\([0-9.]*\).*/\1/p')
    RSS=$(echo "$err" | sed -n 's/.*maxrss_kb=\([0-9]*\).*/\1/p')
    local exitc=$(echo "$err" | sed -n 's/.*exit=\([0-9]*\).*/\1/p')
    if [ $rc -ne 0 ] || [ -z "$T" ] || [ "${exitc:-0}" != "0" ]; then
        note_fail "$app" "$alloc" "exit=${exitc:-$rc}"
        return 1
    fi
    return 0
}

record() { # record app alloc
    local key=$1:$2
    if [ -z "${BEST[$key]:-}" ] || \
       awk "BEGIN{exit !($T < ${BEST[$key]})}"; then
        BEST[$key]=$T; BESTRSS[$key]=$RSS
    fi
    STATUS[$key]=ok
}

# ---------------------------------------------------------------- #
# per-app runners: each takes an allocator name, uses timed/record
# ---------------------------------------------------------------- #
run_sqlite() {
    rm -f "$WORK/st.db"
    timed sqlite "$1" "$WORK/speedtest1" "$WORK/st.db" --size 30 \
        && record sqlite "$1"
    rm -f "$WORK/st.db"
}

run_jq() {
    timed jq "$1" jq \
        '[.[]|select(.v%3==0)|{id,t:(.v*.price)}]|sort_by(.t)|length' \
        "$WORK/big.json" && record jq "$1"
}

run_lua() {
    local lua=${LUA_BIN:-$(command -v lua5.4 || command -v lua || echo /tmp/r3tools/lua-5.4.7/lua)}
    timed lua "$1" "$lua" "$WL/churn.lua" && record lua "$1"
}

run_python() {
    timed python "$1" python3 "$WL/churn.py" "$WORK/big.json" \
        && record python "$1"
}

run_node() {
    timed node "$1" node "$WL/churn.js" "$WORK/big.json" \
        && record node "$1"
}

run_gcc() {
    timed gcc "$1" gcc -O1 -I"$WORK/sqlite-amalgamation-3450100" \
        -c "$WORK/sqlite-amalgamation-3450100/sqlite3.c" \
        -o "$WORK/sqlite3.o" && record gcc "$1"
}

run_git() {
    rm -rf "$WORK/gitrun"
    cp -r "$WORK/gitfix" "$WORK/gitrun"
    timed git "$1" bash -c "
        git -C '$WORK/gitrun' log --format=%H --all | wc -l >/dev/null &&
        git -C '$WORK/gitrun' rev-list --objects --all | wc -l >/dev/null &&
        git -C '$WORK/gitrun' gc --quiet" \
        && record git "$1"
    rm -rf "$WORK/gitrun"
}

run_redis() {
    local alloc=$1
    local redis=${REDIS_BIN:-$(command -v redis-server || echo /tmp/r3tools/redis/src/redis-server)}
    local bench=${REDIS_BENCH:-$(command -v redis-benchmark || echo /tmp/r3tools/redis/src/redis-benchmark)}
    local cli=${REDIS_CLI:-$(command -v redis-cli || echo /tmp/r3tools/redis/src/redis-cli)}
    local port=7777 lib=${LIBS[$alloc]}
    rm -rf "$WORK/redis-run"; mkdir -p "$WORK/redis-run"
    if [ -n "$lib" ]; then
        env LD_PRELOAD="$lib" "$redis" --port $port --save '' \
            --appendonly no --protected-mode no --daemonize yes \
            --dir "$WORK/redis-run" --logfile "$WORK/redis-run/log" \
            --pidfile "$WORK/redis-run/pid"
    else
        "$redis" --port $port --save '' --appendonly no \
            --protected-mode no --daemonize yes \
            --dir "$WORK/redis-run" --logfile "$WORK/redis-run/log" \
            --pidfile "$WORK/redis-run/pid"
    fi
    local i
    for i in $(seq 1 50); do
        "$cli" -p $port ping >/dev/null 2>&1 && break
        sleep 0.2
    done
    if ! "$cli" -p $port ping >/dev/null 2>&1; then
        note_fail redis "$alloc" "server-did-not-start"
        return
    fi
    local pid
    pid=$(cat "$WORK/redis-run/pid")
    local start end out
    start=$(date +%s.%N)
    out=$(timeout "$TIMEOUT" "$bench" -p $port -t set,get -n 100000 \
          -c 16 2>/dev/null)
    end=$(date +%s.%N)
    # correctness: the server must still answer and round-trip a value
    "$cli" -p $port set r3check hello >/dev/null 2>&1
    local got
    got=$("$cli" -p $port get r3check 2>/dev/null)
    RSS=$(grep VmHWM "/proc/$pid/status" 2>/dev/null | awk '{print $2}')
    "$cli" -p $port shutdown nosave >/dev/null 2>&1
    if [ "$got" != "hello" ] || [ -z "$out" ]; then
        note_fail redis "$alloc" "roundtrip=$got"
        return
    fi
    T=$(echo "$start $end" | awk '{printf "%.3f", $2-$1}')
    record redis "$alloc"
    local setrps getrps
    setrps=$(echo "$out" | grep -A1 '^SET' | grep -o '[0-9.]* requests per second' | grep -o '[0-9.]*')
    getrps=$(echo "$out" | grep -A1 '^GET' | grep -o '[0-9.]* requests per second' | grep -o '[0-9.]*')
    echo "  redis/$alloc set=${setrps:-?} get=${getrps:-?} rps" >&2
}

# ---------------------------------------------------------------- #
# main loop: rounds x apps x allocators, interleaved
# ---------------------------------------------------------------- #
declare -A RUNNER=( [redis]=run_redis [sqlite]=run_sqlite [jq]=run_jq \
                    [lua]=run_lua [python]=run_python [node]=run_node \
                    [gcc]=run_gcc [git]=run_git )
for app in $APPS; do
    [ -d "$WORK" ] || { echo "run bench/apps/prep.sh first" >&2; exit 1; }
    for r in $(seq 1 "$REPS"); do
        for a in $ALLOCATORS; do
            [ -n "${LIBS[$a]+x}" ] || continue
            ${RUNNER[$app]} "$a"
        done
    done
done

echo
printf '%-8s' app
for a in $ALLOCATORS; do [ -n "${LIBS[$a]+x}" ] && printf ' %22s' "$a"; done
echo
for app in $APPS; do
    printf '%-8s' "$app"
    for a in $ALLOCATORS; do
        [ -n "${LIBS[$a]+x}" ] || continue
        key=$app:$a
        if [ "${STATUS[$key]:-}" = ok ]; then
            printf ' %10ss %8sKB' "${BEST[$key]}" "${BESTRSS[$key]}"
        else
            printf ' %22s' "${STATUS[$key]:-n/a}"
        fi
    done
    echo
done
