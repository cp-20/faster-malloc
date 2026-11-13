#!/usr/bin/env bash
# bursty cell: bench/apps/bursty.c -- a server-shaped heap that
# ramps, idles, ramps higher, drains and idles again.  The columns
# that matter are RSS at the end of each idle: that is memory the
# allocator either returned to the OS or is still holding for a
# live set of ~10 MB.  The checksum (identical work) must match
# across allocators; RSS columns are from the min-wall run.
set -u
. "$(dirname "$0")/cellib.sh"

printf "%-11s %9s %10s %10s %10s %12s\n" allocator secs rss_peak rss_idle1 rss_idle2 checksum
want_checksum=""
for name in "${NAMES[@]}"; do
  LIB=$(lib_for "$name")
  [ -n "$LIB" ] && [ ! -f "$LIB" ] && { printf "%-11s MISSING\n" "$name"; continue; }
  read -r wall rss <<< "$(run_cell "$APPDIR/bursty")"
  if [ "$wall" = "FAIL" ]; then
    printf "%-11s FAIL\n" "$name"
    continue
  fi
  idle1=$(sed -n 's/^idle1  rss=\([0-9]*\).*/\1/p' "$CELL_OUT_FILE")
  idle2=$(sed -n 's/^idle2  rss=\([0-9]*\).*/\1/p' "$CELL_OUT_FILE")
  cs=$(sed -n 's/.*checksum=\([0-9]*\).*/\1/p' "$CELL_OUT_FILE")
  if [ -z "$want_checksum" ]; then
    want_checksum=$cs
  elif [ "$cs" != "$want_checksum" ]; then
    printf "%-11s CHECKSUM MISMATCH (%s != %s)\n" "$name" "$cs" "$want_checksum"
    continue
  fi
  printf "%-11s %9s %10s %10s %10s %12s\n" "$name" "$wall" "$rss" "$idle1" "$idle2" "$cs"
done
