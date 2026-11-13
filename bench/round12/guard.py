"""Alternating app-matrix total times and sustained KV churn, with outputs checked."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time

parser = argparse.ArgumentParser()
parser.add_argument('output')
parser.add_argument('--cpu', action='store_true')
parser.add_argument('--reps', type=int, default=7)
args = parser.parse_args()
root = Path.cwd()
variants = {'base-v2':'work/round12/base/libfastmalloc2.so',
            'v2':'libfastmalloc2.so', 'base-jit':'work/round12/base/libjitmalloc.so',
            'jit':'libjitmalloc.so'}
for path in variants.values():
    assert Path(path).is_file(), path
commands = {'python':['python3', 'bench/apps/workloads/churn.py', 'bench/apps/work/big.json'],
            'node':['node', 'bench/apps/workloads/churn.js', 'bench/apps/work/big.json'],
            'kvstore':['bench/macro', 'kvstore', '3000000']}
expected = {'python':'python-churn checksum 625726', 'node':'node-churn checksum 671898'}
with open(args.output, 'w') as f:
    for rep in range(args.reps):
        names = list(variants)
        if rep % 2:
            names.reverse()
        for app, command in commands.items():
            for name in names:
                env = {k:v for k,v in os.environ.items() if k != 'LD_PRELOAD' and not k.startswith('FM_')}
                lib = str(root / variants[name])
                if args.cpu:
                    # wait4 per target; temp files avoid pipe deadlocks and an
                    # interposed timing wrapper. CPU time includes all threads.
                    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
                        start = time.monotonic()
                        child = subprocess.Popen(command, env=env | {'LD_PRELOAD':lib}, stdout=out, stderr=err)
                        _, status, usage = os.wait4(child.pid, 0)
                        elapsed = time.monotonic() - start
                        child.returncode = os.waitstatus_to_exitcode(status)
                        out.seek(0)
                        err.seek(0)
                        row = dict(rep=rep, app=app, variant=name, stdout=out.read().decode(),
                                   stderr=err.read().decode(), rc=child.returncode,
                                   seconds=elapsed, rss_kb=usage.ru_maxrss,
                                   cpu_seconds=usage.ru_utime + usage.ru_stime)
                    assert row['rc'] == 0, row
                else:
                    r = subprocess.run(['python3', 'bench/apps/run_timed.py', 'env',
                                        'LD_PRELOAD='+lib, *command], env=env, text=True, capture_output=True)
                    m = re.search(r'elapsed=([\d.]+) maxrss_kb=(\d+) exit=(\d+)', r.stderr)
                    row = dict(rep=rep, app=app, variant=name, stdout=r.stdout, stderr=r.stderr, rc=r.returncode)
                    assert r.returncode == 0 and m, row
                    row.update(seconds=float(m[1]), rss_kb=int(m[2]))
                if app in expected:
                    assert row['stdout'].strip() == expected[app], row
                else:
                    j = json.loads(row['stdout'])
                    assert j['scenario'] == 'kvstore' and j['ops'] > 0, row
                    row['ns_per_op'] = j['ns_per_op']
                f.write(json.dumps(row)+'\n')
                f.flush()
        print('guard round', rep, 'complete', flush=True)
