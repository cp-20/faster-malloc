#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
results=${RESULTS_DIR:-$PWD/bench/results/round10-lock-cost}
mkdir -p "$results"
flags='-O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc'
cc $flags -pthread -Wl,-z,nodelete bench/round10/census-busy.c -o work/lock-cost/census-busy
timeout 30 work/lock-cost/census-busy > "$results/census-busy.txt" 2>&1
for mode in blocking default; do
    define=()
    if [[ $mode == blocking ]]; then define=(-DFM_CENSUS_TRYLOCK=0); fi
    make -B -j4 CFLAGS="$flags ${define[*]}" > "$results/$mode-build.txt" 2>&1
    cat > work/lock-cost/verify-cc <<CC
#!/usr/bin/env bash
exec cc ${define[*]} "\$@"
CC
    chmod +x work/lock-cost/verify-cc
    CC="$PWD/work/lock-cost/verify-cc" bash bench/round7.sh > "$results/$mode-battery.txt" 2>&1
    for impl in static jit; do
        sources=("src/fm_$impl.c")
        if [[ $impl == jit ]]; then sources+=(src/jit_stubs.S); fi
        lib="$PWD/work/lock-cost/debug-$mode-$impl.so"
        cc $flags -DFM_DEBUG "${define[@]}" -shared -Wl,-z,nodelete "${sources[@]}" -o "$lib" -pthread
        for rep in 1 2 3; do
            echo "$mode $impl $rep" >> "$results/debug.txt"
            FM_RELEASE_AGE_MS=0 FM_PURGE_MIN_PAGES=1 FM_IDLE_RELEASE=1 FM_REALLOC_CACHE_SEARCH=1 LD_PRELOAD="$lib" timeout 180 ./bench/stress >> "$results/debug.txt" 2>&1
        done
    done
done
make -B -j4 > "$results/restored-build.txt" 2>&1
