#!/usr/bin/env bash
# Reuse round 10's API usable-byte accounting; never use its timings as a guard.
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 bench/round10-shortlived/make_diag.py
flags='-O3 -g -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc'
instrument_core() {
python3 - <<'PY'
from pathlib import Path
p = Path('work/fm_core_diag.h')
s = p.read_text()
s = s.replace('static inline FM_Batch *fm_batch_alloc(void)', '_Atomic size_t diag_batch_pages, diag_census_calls;\nstatic inline FM_Batch *fm_batch_alloc(void)')
s = s.replace('    FM_Batch *first = (FM_Batch *)page;', '    atomic_fetch_add(&diag_batch_pages, 1);\n    FM_Batch *first = (FM_Batch *)page;')
s = s.replace('    c->cap = 256;', '    atomic_fetch_add(&diag_census_calls, 1);\n    c->cap = 256;')
p.write_text(s)
PY
}
instrument_core
cc $flags -shared -Wl,-z,nodelete work/fm_static_diag.c -o work/round12/diag.so
python3 - <<'PY'
import subprocess
from pathlib import Path
s = subprocess.check_output(['git','show','3d1e114:src/fm_core.h'],text=True)
s = s.replace('sp->lnext = (uintptr_t)fm_lcache_head[b];',
              'sp->user_size = 0; /* diagnostic cached marker */\n            sp->lnext = (uintptr_t)fm_lcache_head[b];')
Path('work/fm_core_diag.h').write_text(s)
PY
instrument_core
cc $flags -shared -Wl,-z,nodelete work/fm_static_diag.c -o work/round12/base-diag.so
python3 bench/round12/attribute.py bench/results/round12-startup/live-attribution.jsonl --diag
