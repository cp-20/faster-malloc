#!/usr/bin/env bash
# Full correctness battery; run from any directory. Stop on first failure.
set -euo pipefail
cd "$(dirname "$0")/.."
make -j4
for lib in libfastmalloc2.so libjitmalloc.so; do
    export LD_PRELOAD="$PWD/$lib"
    echo "=== $lib ==="
    ./test_basic
    for profile in balanced throughput footprint; do
        FM_PROFILE=$profile ./test_basic
    done
    for rep in 1 2 3; do ./bench/stress; done
    ./bench/check
    ./bench/mt 0
    ./bench/mt 1
    for mode in 0 1; do
        FM_REALLOC_CACHE_SEARCH=$mode ./bench/realloccache "$mode"
        FM_IDLE_RELEASE=$mode ./bench/idlefork "$mode"
    done
    FM_REALLOC_CACHE_SEARCH=1 FM_IDLE_RELEASE=1 ./test_basic
    FM_REALLOC_CACHE_SEARCH=1 FM_IDLE_RELEASE=1 FM_PURGE_MIN_PAGES=1 ./bench/stress
    unset LD_PRELOAD
done
bash bench/earlyinit.sh
bash bench/tlsexit.sh
bash bench/idlelifecycle.sh
bash bench/round9/grace.sh
