#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
results="$PWD/bench/results/round12/final-verification"
mkdir -p "$results" work/round12
flags='-O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc'
for debug in ordinary debug; do
    extra=()
    if [[ $debug == debug ]]; then extra=(-DFM_DEBUG); fi
    make -B -j4 CFLAGS="$flags ${extra[*]}" > "$results/$debug-build.txt" 2>&1
    cat > work/round12/final-cc <<CC
#!/usr/bin/env bash
exec cc ${extra[*]} "\$@"
CC
    chmod +x work/round12/final-cc
    CC="$PWD/work/round12/final-cc" bash bench/round7.sh > "$results/$debug-battery.txt" 2>&1
done
make -B -j4 > "$results/restored-build.txt" 2>&1
