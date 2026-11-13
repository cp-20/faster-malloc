#!/bin/bash
# Run after all timing jobs; use the diagnostic library built by diagnostics.sh.
set -euo pipefail
for rep in 2 3; do
  for scenario in local cross many census; do
    echo "rep=$rep case=$scenario"
    case $scenario in
      local) cmd=(bench/mt 0 10000000 8);;
      cross) cmd=(bench/mt 1 10000000 8);;
      many) cmd=(work/ownership 200000 0);;
      census) cmd=(work/ownership 200000 1);;
    esac
    LD_PRELOAD=$PWD/work/lockprice.so "${cmd[@]}"
  done
done
