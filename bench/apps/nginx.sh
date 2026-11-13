#!/usr/bin/env bash
# nginx cell: static-file serving under keep-alive HTTP load
# (bench/apps/wrk.c), 2 workers, 20 connections, 10 s per run.
# Reports requests/sec, p50/p99 latency, and peak summed RSS of
# the nginx processes (sampled from /proc during the load).
# Correctness: wrk verifies response framing + Content-Length and
# must report 0 errors, and a fetched body is diffed per run.
set -u
. "$(dirname "$0")/cellib.sh"
PORT=18099
WORK=$(mktemp -d /tmp/fm-nginx-XXXXXX)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$WORK/www"
# nginx workers run as www-data: the docroot must be traversable.
chmod 755 "$WORK" "$WORK/www"
python3 - "$WORK/www" <<'EOF'
import sys
d = sys.argv[1]
small = ("<!doctype html><html><head><title>fm</title></head><body>" +
         "<p>faster-malloc nginx cell</p>" * 12 + "</body></html>")
open(d + "/small.html", "w").write(small)
row = "0123456789abcdef" * 64
open(d + "/big.html", "w").write("\n".join([row] * 30))
EOF
chmod 644 "$WORK/www/small.html" "$WORK/www/big.html"

cat > "$WORK/nginx.conf" <<EOF
daemon off;
worker_processes 2;
pid $WORK/nginx.pid;
error_log $WORK/error.log warn;
events { worker_connections 256; }
http {
  access_log off;
  client_body_temp_path $WORK/body;
  proxy_temp_path $WORK/proxy;
  fastcgi_temp_path $WORK/fastcgi;
  uwsgi_temp_path $WORK/uwsgi;
  scgi_temp_path $WORK/scgi;
  sendfile on;
  # nginx closes a keep-alive connection after 1000 requests by
  # default; wrk drives ~3500 requests per connection in 10 s and
  # (correctly) counts the mid-run close as an error.  Raise the
  # cap far above the run so the cell measures the allocator, not
  # nginx's connection policy.  (This was the round-5 "all FAIL"
  # nginx result: exactly 20 errors = 20 connections x 1 close.)
  keepalive_requests 1000000;
  server {
    listen 127.0.0.1:$PORT;
    root $WORK/www;
  }
}
EOF

# An earlier aborted run can leave a master behind, still bound
# to $PORT and serving a docroot that no longer exists; the port
# wait below would then succeed against the stale server and the
# body check would fail for every allocator.  Kill any master
# started from a /tmp/fm-nginx-* prefix before we begin.
pkill -f "nginx: master process nginx -c /tmp/fm-nginx-" 2>/dev/null
sleep 0.5

printf "%-11s %10s %9s %9s %10s %7s\n" allocator rps p50_us p99_us peak_rss errors
for name in "${NAMES[@]}"; do
  LIB=$(lib_for "$name")
  [ -n "$LIB" ] && [ ! -f "$LIB" ] && { printf "%-11s MISSING\n" "$name"; continue; }
  best=""
  for ((r = 0; r < REPS; r++)); do
    rm -f "$WORK/error.log"
    if [ -n "$LIB" ]; then
      LD_PRELOAD="$LIB" nginx -c "$WORK/nginx.conf" -p "$WORK" -e "$WORK/startup.log" &
    else
      nginx -c "$WORK/nginx.conf" -p "$WORK" -e "$WORK/startup.log" &
    fi
    NG=$!
    # wait for the port
    ok=0
    for i in $(seq 1 50); do
      if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then
        exec 3>&- 3<&-; ok=1; break
      fi
      sleep 0.2
    done
    if [ $ok -ne 1 ]; then
      echo "nginx failed to start for $name" >&2
      tail -3 "$WORK/error.log" >&2 2>/dev/null
      kill $NG 2>/dev/null; wait $NG 2>/dev/null
      best="FAIL"
      break
    fi
    # correctness: body must match the file exactly
    got=$(curl -s "http://127.0.0.1:$PORT/small.html" 2>/dev/null)
    want=$(cat "$WORK/www/small.html")
    if [ "$got" != "$want" ]; then
      echo "nginx body mismatch for $name" >&2
      kill $NG 2>/dev/null; wait $NG 2>/dev/null
      best="FAIL"
      break
    fi
    # peak summed RSS sampler (master + its workers only, so a
    # foreign nginx on the box is never counted)
    ( peak=0
      while kill -0 $NG 2>/dev/null; do
        s=0
        for p in $NG $(pgrep -P $NG 2>/dev/null); do
          v=$(awk '/VmRSS/{print $2}' "/proc/$p/status" 2>/dev/null)
          [ -n "$v" ] && s=$((s + v))
        done
        [ $s -gt $peak ] && peak=$s
        echo $peak > "$WORK/peak.next"
        mv "$WORK/peak.next" "$WORK/peak"
        sleep 0.2
      done ) &
    SAMPLER=$!
    json=$("$APPDIR/wrk" -t 2 -c 20 -d 10 -p $PORT /small.html /big.html 2>/dev/null)
    wrc=$?
    kill $NG 2>/dev/null; wait $NG 2>/dev/null
    kill $SAMPLER 2>/dev/null; wait $SAMPLER 2>/dev/null
    peak=$(cat "$WORK/peak" 2>/dev/null || echo 0)
    rps=$(echo "$json" | sed -n 's/.*"rps":\([0-9.]*\).*/\1/p')
    p50=$(echo "$json" | sed -n 's/.*"p50_us":\([0-9]*\).*/\1/p')
    p99=$(echo "$json" | sed -n 's/.*"p99_us":\([0-9]*\).*/\1/p')
    errs=$(echo "$json" | sed -n 's/.*"errors":\([0-9]*\).*/\1/p')
    if [ $wrc -ne 0 ] || [ "$errs" != "0" ] || [ -z "$rps" ]; then
      echo "wrk errors for $name: rc=$wrc json=$json" >&2
      best="FAIL"
      break
    fi
    row="$rps $p50 $p99 $peak $errs"
    if [ -z "$best" ] || awk "BEGIN{exit !($rps > ${best%% *})}"; then
      best=$row
    fi
  done
  if [ "$best" = "FAIL" ] || [ -z "$best" ]; then
    printf "%-11s FAIL\n" "$name"
  else
    printf "%-11s %10s %9s %9s %10s %7s\n" "$name" $best
  fi
done
