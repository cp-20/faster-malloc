#!/usr/bin/env bash
# pgbench cell: PostgreSQL 16 + pgbench -- a second database shape
# (multi-process server, shared buffers + per-backend heaps, OLTP
# read/write mix) next to redis (single-threaded event loop) and
# sqlite (embedded).  The server runs as user `nobody` (postgres
# refuses root); LD_PRELOAD crosses the su boundary via the
# environment.  Data is initialized once under glibc; each
# allocator then serves the same 15 s pgbench run (scale 5,
# 4 clients, 2 jobs).  Reports pgbench tps, average latency, and
# peak summed RSS of the postgres processes.  Correctness: the
# pgbench run must complete with 0 failed transactions.
#
# REPS defaults to 1 here: server startup + a 15 s run per
# allocator is already the matrix's slowest cell.
set -u
. "$(dirname "$0")/cellib.sh"
REPS=${PGREPS:-1}
PGBIN=$(ls -d /usr/lib/postgresql/*/bin | sort -V | tail -1)
PORT=15433
WORK=$(mktemp -d /tmp/fm-pg-XXXXXX)
cleanup() {
  su nobody -s /bin/bash -c "$PGBIN/pg_ctl -D $WORK/data -m immediate -w stop" >/dev/null 2>&1
  rm -rf "$WORK"
}
trap cleanup EXIT
chown nobody:nogroup "$WORK"

as_nobody() { su nobody -s /bin/bash -c "$1"; }

as_nobody "$PGBIN/initdb -D $WORK/data -A trust --no-instructions" >/dev/null 2>&1 || {
  echo "initdb failed" >&2; exit 1; }

start_server() { # $1 = lib (may be empty)
  if [ -n "$1" ]; then
    as_nobody "LD_PRELOAD=$1 $PGBIN/pg_ctl -D $WORK/data -l $WORK/log -o '-k $WORK -p $PORT -c shared_buffers=32MB -c fsync=off -c synchronous_commit=off -c full_page_writes=off -c autovacuum=off -c max_connections=16' -w -t 30 start" >/dev/null 2>&1
  else
    as_nobody "$PGBIN/pg_ctl -D $WORK/data -l $WORK/log -o '-k $WORK -p $PORT -c shared_buffers=32MB -c fsync=off -c synchronous_commit=off -c full_page_writes=off -c autovacuum=off -c max_connections=16' -w -t 30 start" >/dev/null 2>&1
  fi
}
stop_server() {
  as_nobody "$PGBIN/pg_ctl -D $WORK/data -m fast -w -t 30 stop" >/dev/null 2>&1
}

# initialize the pgbench schema once, under glibc
start_server "" || { echo "initial server start failed" >&2; exit 1; }
"$PGBIN/pgbench" -h "$WORK" -p $PORT -U nobody -d postgres -i -s 5 -q >/dev/null 2>&1 || {
  # pgbench connects as the invoking user by default; create via socket as nobody db user
  as_nobody "$PGBIN/pgbench -h $WORK -p $PORT -d postgres -i -s 5 -q" >/dev/null 2>&1 || {
    echo "pgbench -i failed" >&2; exit 1; }
}
stop_server

printf "%-11s %10s %11s %12s %8s\n" allocator tps avg_ms peak_rss failed
for name in "${NAMES[@]}"; do
  LIB=$(lib_for "$name")
  [ -n "$LIB" ] && [ ! -f "$LIB" ] && { printf "%-11s MISSING\n" "$name"; continue; }
  best=""
  for ((r = 0; r < REPS; r++)); do
    start_server "$LIB" || { best="FAIL"; break; }
    # peak summed RSS sampler over the postgres process group
    ( peak=0
      while :; do
        s=0; any=0
        for p in $(pgrep -u nobody postgres 2>/dev/null); do
          v=$(awk '/VmRSS/{print $2}' "/proc/$p/status" 2>/dev/null)
          [ -n "$v" ] && { s=$((s + v)); any=1; }
        done
        [ $any -eq 0 ] && break
        [ $s -gt $peak ] && peak=$s
        echo $peak > "$WORK/peak"
        sleep 0.5
      done ) &
    SAMPLER=$!
    out=$(as_nobody "$PGBIN/pgbench -h $WORK -p $PORT -d postgres -c 4 -j 2 -T 15 --no-vacuum" 2>&1)
    kill $SAMPLER 2>/dev/null; wait $SAMPLER 2>/dev/null
    stop_server
    tps=$(echo "$out" | sed -n 's/.*tps = \([0-9.]*\).*/\1/p' | tail -1)
    avg=$(echo "$out" | sed -n 's/.*latency average = \([0-9.]*\) ms.*/\1/p')
    failed=$(echo "$out" | sed -n 's/.*number of failed transactions: \([0-9]*\).*/\1/p')
    peak=$(cat "$WORK/peak" 2>/dev/null || echo 0)
    if [ -z "$tps" ] || [ "$failed" != "0" ]; then
      echo "pgbench run failed for $name (failed=$failed):" >&2
      echo "$out" | tail -4 >&2
      best="FAIL"
      break
    fi
    row="$tps $avg $peak $failed"
    if [ -z "$best" ] || awk "BEGIN{exit !($tps > ${best%% *})}"; then
      best=$row
    fi
  done
  if [ "$best" = "FAIL" ] || [ -z "$best" ]; then
    printf "%-11s FAIL\n" "$name"
  else
    printf "%-11s %10s %11s %12s %8s\n" "$name" $best
  fi
done
