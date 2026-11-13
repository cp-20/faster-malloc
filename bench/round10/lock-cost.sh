#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p work/lock-cost/{base,stripe} bench/results/round10-lock-cost
flags=(-O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter)
for file in fm_core.h fm_static.c size_classes2.h; do
    git show "f90550c:src/$file" > "work/lock-cost/base/$file"
    cp "work/lock-cost/base/$file" "work/lock-cost/stripe/$file"
done
mkdir -p work/lock-cost/stripe/src
cp work/lock-cost/stripe/fm_core.h work/lock-cost/stripe/src/
(cd work/lock-cost/stripe && patch -p1 < ../../../bench/round10/central-stripes.patch)
cp work/lock-cost/stripe/src/fm_core.h work/lock-cost/stripe/fm_core.h
cc "${flags[@]}" -Iwork/lock-cost/base -shared -Wl,-z,nodelete work/lock-cost/base/fm_static.c -o work/lock-cost/base.so -pthread
cc "${flags[@]}" -Iwork/lock-cost/stripe -shared -Wl,-z,nodelete -DFM_CENTRAL_STRIPES work/lock-cost/stripe/fm_static.c -o work/lock-cost/stripe.so -pthread
cc "${flags[@]}" -Isrc -shared -Wl,-z,nodelete -DFM_CENSUS_TRYLOCK src/fm_static.c -o work/lock-cost/adaptive.so -pthread
cc -O2 -g bench/ownership.c -o work/lock-cost/ownership -pthread
make -j4
python3 bench/round10/lock-cost.py stripe
python3 bench/round10/lock-cost.py adaptive
