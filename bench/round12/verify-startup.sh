#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
results="$PWD/bench/results/round12-startup"
mkdir -p "$results" work/round12
flags='-O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc'
trap 'make -B -j4 > "$results/restored-build.log" 2>&1' EXIT
: > "$results/debug-purge.log"
for debug in release debug; do
    extra=()
    if [[ $debug == debug ]]; then extra=(-DFM_DEBUG); fi
    cc $flags "${extra[@]}" -pthread -Wl,-z,nodelete bench/round12/smallspan.c -o work/round12/smallspan
    FM_PURGE_MIN_PAGES=1 work/round12/smallspan > "$results/$debug-smallspan.log" 2>&1
    make -B -j4 CFLAGS="$flags ${extra[*]}" > "$results/$debug-build.log" 2>&1
    cat > work/round12/verify-cc <<CC
#!/usr/bin/env bash
exec cc ${extra[*]} "\$@"
CC
    chmod +x work/round12/verify-cc
    CC="$PWD/work/round12/verify-cc" bash bench/round7.sh > "$results/$debug-round7.log" 2>&1
    if [[ $debug == debug ]]; then
        for impl in fastmalloc2 jitmalloc; do
            for rep in 1 2 3; do
                echo "$impl $rep" >> "$results/debug-purge.log"
                FM_RELEASE_AGE_MS=0 FM_PURGE_MIN_PAGES=1 FM_IDLE_RELEASE=1 FM_REALLOC_CACHE_SEARCH=1 \
                    LD_PRELOAD="$PWD/lib$impl.so" timeout 180 bench/stress >> "$results/debug-purge.log" 2>&1
            done
        done
    fi
done
