#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
cc=${CC:-cc}
# These allocators use initial-exec TLS; reserve room for deliberate late loading.
export GLIBC_TUNABLES=glibc.rtld.optional_static_tls=131072
"$cc" -O2 -g -Wall -Wextra bench/idlelifecycle.c -o "$TMP/lifecycle" -ldl -pthread
for lib in libfastmalloc2.so libjitmalloc.so; do
    readelf -d "$lib" | grep -q NODELETE
    for enabled in 0 1; do
        FM_IDLE_RELEASE=$enabled "$TMP/lifecycle" "$PWD/$lib" "$enabled"
    done
done
for impl in static jit; do
    sources=("src/fm_$impl.c")
    if [[ $impl == jit ]]; then sources+=(src/jit_stubs.S); fi
    for fault in atfork create retry; do
        case $fault in
            atfork) define=-DFM_TEST_IDLE_ATFORK_FAIL; expected=0 ;;
            create) define=-DFM_TEST_IDLE_CREATE_FAILURES=-1; expected=0 ;;
            retry) define=-DFM_TEST_IDLE_CREATE_FAILURES=1; expected=1 ;;
        esac
        lib="$TMP/$impl-$fault.so"
        "$cc" -O2 -g -fPIC -shared -Isrc -Wall -Wextra -Wno-unused-parameter \
            -Wl,-z,nodelete "$define" "${sources[@]}" -o "$lib" -pthread
        if [[ $fault != atfork ]]; then
            FM_IDLE_RELEASE=1 LD_PRELOAD="$lib" ./bench/idlefork "$expected"
        fi
        FM_IDLE_RELEASE=1 "$TMP/lifecycle" "$lib" "$expected"
    done
done
