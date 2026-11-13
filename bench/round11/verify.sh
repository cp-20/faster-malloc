#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p work/lock-cost work/round11
RESULTS_DIR="$PWD/bench/results/round11-crossfree/verification" bash bench/round10/lock-cost-verify.sh
results="$PWD/bench/results/round11-crossfree/verification"
flags='-O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc'
trap 'make -B -j4 > "$results/final-restored-build.txt" 2>&1' EXIT
make -B -j4 CFLAGS="$flags -DFM_DEBUG" > "$results/full-debug-build.txt" 2>&1
cat > work/round11/debug-cc <<'CC'
#!/usr/bin/env bash
exec cc -DFM_DEBUG "$@"
CC
chmod +x work/round11/debug-cc
CC="$PWD/work/round11/debug-cc" bash bench/round7.sh > "$results/full-debug-battery.txt" 2>&1
