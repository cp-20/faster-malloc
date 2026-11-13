#!/usr/bin/env bash
# ruby cell: bench/apps/bench.rb under each allocator.  Time and
# peak RSS follow apps.sh conventions; the checksum must be
# identical for every allocator.
set -u
. "$(dirname "$0")/cellib.sh"

printf "%-11s %9s %11s %12s\n" allocator secs peak_rss checksum
want_checksum=""
for name in "${NAMES[@]}"; do
  LIB=$(lib_for "$name")
  [ -n "$LIB" ] && [ ! -f "$LIB" ] && { printf "%-11s MISSING\n" "$name"; continue; }
  read -r wall rss <<< "$(run_cell ruby "$APPDIR/bench.rb")"
  if [ "$wall" = "FAIL" ]; then
    printf "%-11s FAIL\n" "$name"
    continue
  fi
  cs=$(sed -n 's/.*"checksum":\([0-9]*\).*/\1/p' "$CELL_OUT_FILE")
  if [ -z "$want_checksum" ]; then
    want_checksum=$cs
  elif [ "$cs" != "$want_checksum" ]; then
    printf "%-11s CHECKSUM MISMATCH (%s != %s)\n" "$name" "$cs" "$want_checksum"
    continue
  fi
  printf "%-11s %9s %11s %12s\n" "$name" "$wall" "$rss" "$cs"
done
