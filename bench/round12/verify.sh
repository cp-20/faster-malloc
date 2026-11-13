#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
results="$PWD/bench/results/round12/verification"
mkdir -p "$results" work/round12
flags='-O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc'
cc $flags -DFM_DEBUG bench/round12/cold-cache.c -o work/round12/cold-cache -pthread
FM_LCACHE_COLD_PURGE=1 work/round12/cold-cache > "$results/cold-cache.txt" 2>&1
for debug in ordinary debug; do
    extra=()
    if [[ $debug == debug ]]; then extra=(-DFM_DEBUG); fi
    make -B -j4 CFLAGS="$flags ${extra[*]}" > "$results/$debug-build.txt" 2>&1
    cat > work/round12/verify-cc <<CC
#!/usr/bin/env bash
exec cc ${extra[*]} "\$@"
CC
    chmod +x work/round12/verify-cc
    for mode in 0 1; do
        CC="$PWD/work/round12/verify-cc" FM_LCACHE_COLD_PURGE=$mode bash bench/round7.sh > "$results/$debug-mode$mode-battery.txt" 2>&1
    done
done
make -B -j4 > "$results/restored-build.txt" 2>&1
