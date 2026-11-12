#!/bin/bash
# tlsexit.sh -- check that exiting threads return allocator-owned caches.
# glibc and v1 are informational controls; v2 and jit must meet the RSS bound.
set -u
cd "$(dirname "$0")/.."
TMP=$(mktemp -d) || exit 1
trap 'rm -rf "$TMP"' EXIT

${CC:-cc} -O2 -g -Wall -Wextra -o "$TMP/tlsexit" bench/tlsexit.c \
    -pthread || exit 1

fail=0
for spec in "glibc:" "v1:./libfastmalloc.so" "v2:./libfastmalloc2.so" \
            "jit:./libjitmalloc.so"; do
    name=${spec%%:*}
    lib=${spec#*:}
    if [ -n "$lib" ]; then
        out=$(LD_PRELOAD="$PWD/$lib" "$TMP/tlsexit" 2>&1)
        status=$?
    else
        out=$(LD_PRELOAD= "$TMP/tlsexit" 2>&1)
        status=$?
    fi
    case "$name" in
        glibc|v1)
            printf '%-6s informational (exit %d): %s\n' "$name" "$status" "$out"
            ;;
        *)
            if [ "$status" -eq 0 ]; then
                printf '%-6s ok: %s\n' "$name" "$out"
            else
                printf '%-6s FAIL (exit %d): %s\n' "$name" "$status" "$out"
                fail=1
            fi
            ;;
    esac
done
exit $fail
