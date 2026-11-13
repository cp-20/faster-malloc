#!/usr/bin/env bash
# Full baseline/prototype battery, including custom-build lifecycle faults.
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p work/reclamation bench/results/round9-reclamation
results=$PWD/bench/results/round9-reclamation
base_flags='-O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc'
restore() { make -B -j4 CFLAGS="$base_flags" > "$results/restore.txt" 2>&1; }
trap restore EXIT
make -B -j4 CFLAGS="$base_flags" > "$results/build.txt" 2>&1
bash bench/round7.sh > "$results/locked-battery.txt" 2>&1
# Scripts compiling custom allocators use CC; propagate the prototype there too.
cat > work/reclamation/epoch-cc <<'CC'
#!/usr/bin/env bash
exec cc -DFM_RECLAIM_EPOCH "$@"
CC
chmod +x work/reclamation/epoch-cc
make -B -j4 CFLAGS="$base_flags -DFM_RECLAIM_EPOCH" > "$results/epoch-build.txt" 2>&1
CC="$PWD/work/reclamation/epoch-cc" bash bench/round7.sh > "$results/epoch-battery.txt" 2>&1
cc -O2 -g -Isrc -pthread -Wl,-z,nodelete bench/round9/epoch.c -o work/reclamation/epoch-test
timeout 30 work/reclamation/epoch-test > "$results/grace.txt" 2>&1
: > "$results/debug.txt"
for mode in locked epoch; do
    define=()
    if [[ $mode == epoch ]]; then define=(-DFM_RECLAIM_EPOCH); fi
    for impl in static jit; do
        sources=("src/fm_$impl.c")
        if [[ $impl == jit ]]; then sources+=(src/jit_stubs.S); fi
        lib="$PWD/work/reclamation/debug-$mode-$impl.so"
        cc $base_flags -DFM_DEBUG "${define[@]}" -shared -Wl,-z,nodelete \
            "${sources[@]}" -o "$lib" -pthread
        for rep in 1 2 3; do
            echo "$mode $impl $rep" >> "$results/debug.txt"
            FM_RELEASE_AGE_MS=0 FM_PURGE_MIN_PAGES=1 FM_IDLE_RELEASE=1 \
                FM_REALLOC_CACHE_SEARCH=1 LD_PRELOAD="$lib" \
                timeout 180 ./bench/stress >> "$results/debug.txt" 2>&1
        done
    done
done
printf 'baseline + epoch batteries and 12 debug runs passed\n' > "$results/verification.txt"
