#!/bin/bash
set -euo pipefail
cc -O2 -shared -fPIC -o work/realloccount.so bench/realloccount.c -ldl
export LD_LIBRARY_PATH=$PWD/work/tools/usr/lib/x86_64-linux-gnu
export RUBYLIB=$PWD/work/tools/usr/lib/ruby/3.3.0:$PWD/work/tools/usr/lib/x86_64-linux-gnu/ruby/3.3.0
{
for app in lua jq ruby python; do
  echo "$app"
  case $app in
    python) cmd=(python3 bench/apps/workloads/churn.py work/big.json);;
    lua) cmd=(work/tools/usr/bin/lua5.4 bench/apps/workloads/churn.lua);;
    jq) cmd=(jq '[.[]|select(.v%3==0)|{id,t:(.v*.price)}]|sort_by(.t)|length' work/big.json);;
    ruby) cmd=(work/tools/usr/bin/ruby3.3 --disable-gems bench/apps/bench.rb);;
  esac
  for mode in 0 1; do
    echo "mode=$mode"
    FM_REALLOC_CACHE_SEARCH=$mode LD_PRELOAD=$PWD/work/realloccount.so:$PWD/libfastmalloc2.so "${cmd[@]}"
  done
done
} >bench/results/round8/realloc-counts.txt 2>&1
cc -O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc -shared \
  -Wl,--wrap=pthread_mutex_lock -o work/lockprice.so src/fm_static.c bench/round8/lockprice.c -ldl
{
nm work/lockprice.so | rg 'fm_(central|census)_lock'
for case in local cross many census; do
  echo "$case"
  case $case in
    local) cmd=(bench/mt 0 10000000 8);;
    cross) cmd=(bench/mt 1 10000000 8);;
    many) cmd=(work/ownership 200000 0);;
    census) cmd=(work/ownership 200000 1);;
  esac
  LD_PRELOAD=$PWD/work/lockprice.so "${cmd[@]}"
done
} >bench/results/round8/lockprice.txt 2>&1
for build in v2 jit; do
  if [ "$build" = v2 ]; then source=(src/fm_static.c); else source=(src/fm_jit.c src/jit_stubs.S); fi
  cc -O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc -DFM_DEBUG -shared -o work/debug-$build.so "${source[@]}"
  for rep in 1 2 3; do
    echo "$build debug stress $rep"
    FM_RELEASE_AGE_MS=0 FM_PURGE_MIN_PAGES=1 FM_IDLE_RELEASE=1 FM_REALLOC_CACHE_SEARCH=1 LD_PRELOAD=$PWD/work/debug-$build.so bench/stress
  done
done >bench/results/round8/debug.txt 2>&1
