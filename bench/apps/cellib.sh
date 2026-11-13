# cellib.sh -- shared helpers for the round-5 app cells (nginx,
# ruby, pgbench, bursty).  Same conventions as apps.sh: the seven
# standard allocators, min-of-N for times, peak RSS from
# /usr/bin/time -v or /proc sampling.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
APPDIR="$ROOT/bench/apps"
REPS=${REPS:-3}
TIME_BIN=${TIME_BIN:-/usr/bin/time}
read -ra NAMES <<< "${CELL_ALLOCATORS:-glibc v1 v2 jit jemalloc mimalloc tcmalloc}"

sys_lib() {
  ldconfig -p | grep -m1 "$1" | sed 's/.*=> //'
}

lib_for() {
  case "$1" in
    glibc)    echo "" ;;
    v1)       echo "$ROOT/libfastmalloc.so" ;;
    v2)       echo "$ROOT/libfastmalloc2.so" ;;
    jit)      echo "$ROOT/libjitmalloc.so" ;;
    jemalloc) echo "${JEMALLOC_LIB:-$(sys_lib libjemalloc.so)}" ;;
    mimalloc) echo "${MIMALLOC_LIB:-$(sys_lib libmimalloc.so)}" ;;
    tcmalloc) echo "${TCMALLOC_LIB:-$(sys_lib libtcmalloc_minimal.so)}" ;;
  esac
}

# run_cell CMD... -> runs under /usr/bin/time -v; echoes
# "wall_seconds peak_rss_kb" of the best (min-wall) of $REPS runs;
# the best run's stdout is written to $CELL_OUT_FILE (callers read
# it after the call -- a variable would not survive the subshell).
CELL_OUT_FILE=${CELL_OUT_FILE:-/tmp/cell-out-$$.txt}
run_cell() {
  local best_wall="" best_rss="" r
  : > "$CELL_OUT_FILE"
  for ((r = 0; r < REPS; r++)); do
    local out err rc
    out=$(LD_PRELOAD="$LIB" "$TIME_BIN" -v "$@" 2>/tmp/cell-$$.err)
    rc=$?
    err=$(cat /tmp/cell-$$.err)
    if [ $rc -ne 0 ]; then
      echo "RUN FAILED (rc=$rc): $*" >&2
      echo "$err" | tail -3 >&2
      echo "FAIL FAIL"
      return 1
    fi
    local wall rss
    wall=$(echo "$err" | sed -n 's/.*Elapsed (wall clock) time.*: \([0-9.:]*\).*/\1/p')
    rss=$(echo "$err" | sed -n 's/.*Maximum resident set size (kbytes): \([0-9]*\).*/\1/p')
    wall=$(echo "$wall" | awk -F: '{if (NF==2) printf "%.3f", $1*60+$2; else print $1}')
    if [ -z "$best_wall" ] || awk "BEGIN{exit !($wall < $best_wall)}"; then
      best_wall=$wall; best_rss=$rss
      printf '%s' "$out" > "$CELL_OUT_FILE"
    fi
  done
  rm -f /tmp/cell-$$.err
  echo "$best_wall $best_rss"
}
