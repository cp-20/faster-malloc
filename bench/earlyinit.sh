#!/bin/bash
# earlyinit.sh -- regression test for allocation before the
# allocator's own constructor runs (round 3, ADR 0016).
#
# Builds a program whose dependency library allocates from its
# constructor, then runs it under every allocator.  All must print
# "earlyinit ok".
set -u
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

cc -O2 -fPIC -shared -o "$TMP/libearly.so" bench/early_lib.c || exit 1
cc -O2 -o "$TMP/early_main" bench/early_main.c -L"$TMP" -learly \
    -Wl,-rpath,"$TMP" || exit 1

fail=0
for spec in "glibc:" "v1:./libfastmalloc.so" "v2:./libfastmalloc2.so" \
            "jit:./libjitmalloc.so"; do
    name=${spec%%:*}
    lib=${spec#*:}
    if [ -n "$lib" ]; then
        out=$(LD_PRELOAD="$PWD/$lib" "$TMP/early_main" 2>&1)
    else
        out=$("$TMP/early_main" 2>&1)
    fi
    if [ "$out" = "earlyinit ok" ]; then
        printf 'earlyinit %-6s ok\n' "$name"
    else
        printf 'earlyinit %-6s FAIL (%s)\n' "$name" "$out"
        fail=1
    fi
done
exit $fail
