"""Serial startup reproduction, plus target-only timing (no preloaded driver)."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

p = argparse.ArgumentParser()
p.add_argument('output')
p.add_argument('--candidate', action='store_true')
p.add_argument('--reps', type=int, default=7)
a = p.parse_args()
root = Path.cwd()
libs = {'glibc': '', 'base-v2': 'work/round12/base/libfastmalloc2.so',
        'base-jit': 'work/round12/base/libjitmalloc.so',
        'jemalloc': '/lib/x86_64-linux-gnu/libjemalloc.so.2'}
if a.candidate:
    libs.update(v2='libfastmalloc2.so', jit='libjitmalloc.so')
for name, path in [('mimalloc', 'work/libmimalloc.so')]:
    if Path(path).exists():
        libs[name] = path
for path in libs.values():
    assert not path or Path(path).is_file(), path
with open(a.output, 'w') as f:
    for rep in range(a.reps):
        names = list(libs)
        names = names[rep % len(names):] + names[:rep % len(names)]
        for app, cmd in [('python', ['python3', '-c', 'pass']),
                         ('node', ['node', '-e', ''])]:
            for name in names:
                lib = str(root / libs[name]) if libs[name] else ''
                for method in ['round10', 'target-only']:
                    env = {k: v for k, v in os.environ.items()
                           if not k.startswith('FM_') and k != 'LD_PRELOAD'}
                    if method == 'round10':
                        if lib:
                            env['LD_PRELOAD'] = lib
                        command = ['python3', 'bench/apps/run_timed.py', *cmd]
                    else:
                        command = ['python3', 'bench/apps/run_timed.py',
                                   'env', 'LD_PRELOAD=' + lib, *cmd]
                    r = subprocess.run(command, env=env, text=True, capture_output=True)
                    m = re.search(r'elapsed=([\d.]+) maxrss_kb=(\d+) exit=(\d+)', r.stderr)
                    assert r.returncode == 0 and m and 'ERROR: ld.so' not in r.stderr, (command, r.stderr)
                    row = dict(rep=rep, app=app, variant=name, method=method,
                               seconds=float(m[1]), rss_kb=int(m[2]), rc=r.returncode)
                    f.write(json.dumps(row) + '\n')
                    f.flush()
