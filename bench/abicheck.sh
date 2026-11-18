#!/usr/bin/env bash
# abicheck.sh -- aligned allocation + C++ new/delete semantics on v2 and JIT.
set -u
cd "$(dirname "$0")/.."
gcc -O1 -o bench/aligncheck bench/aligncheck.c || exit 1
g++ -O2 -std=c++17 -fsized-deallocation -pthread -o bench/cxxcheck bench/cxxcheck.cpp || exit 1
rc=0
for lib in libfastmalloc2.so libjitmalloc.so; do
  for t in bench/aligncheck bench/cxxcheck; do
    printf '%-18s ' "$lib"; LD_PRELOAD=$PWD/$lib timeout 120 $t || rc=1
  done
done
exit $rc
