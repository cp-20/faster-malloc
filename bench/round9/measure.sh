#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p work/reclamation/base bench/results/round9-reclamation
flags=(-O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter)
for file in fm_core.h fm_static.c size_classes2.h; do
    git show "d9c4364:src/$file" > "work/reclamation/base/$file"
done
cc "${flags[@]}" -Iwork/reclamation/base -shared -Wl,-z,nodelete \
    work/reclamation/base/fm_static.c -o work/reclamation/locked.so -pthread
cc "${flags[@]}" -Isrc -shared -Wl,-z,nodelete -DFM_RECLAIM_EPOCH \
    src/fm_static.c -o work/reclamation/epoch.so -pthread
cc -O2 -g bench/ownership.c -o work/reclamation/ownership -pthread
cc -O2 bench/apps/bursty.c -o work/reclamation/bursty
make -j4
# Normal-path machine code must still match the original locked baseline.
objcopy --dump-section .text=work/reclamation/default.text libfastmalloc2.so
objcopy --dump-section .text=work/reclamation/original.text work/reclamation/locked.so
cmp work/reclamation/default.text work/reclamation/original.text
{
    git rev-parse HEAD
    uname -a
    lscpu
    cat /sys/fs/cgroup/cpu.max
    cc --version
    sha256sum work/reclamation/default.text work/reclamation/original.text
} > bench/results/round9-reclamation/host.txt
python3 bench/round9/measure.py
