import subprocess, os, json, re, time
from pathlib import Path
root = Path.cwd()
dest = root / 'bench/results/round10-shortlived'
variants = {'glibc': ({}, ''), 'jemalloc': ({}, '/lib/x86_64-linux-gnu/libjemalloc.so.2'), 'mimalloc': ({}, str(root / 'work/libmimalloc.so')), 'v2': ({}, str(root / 'libfastmalloc2.so')), 'footprint': ({'FM_PROFILE': 'footprint'}, str(root / 'libfastmalloc2.so')), 'shortlived': ({'FM_PROFILE': 'shortlived'}, str(root / 'libfastmalloc2.so')), 'eager': ({'FM_PROFILE': 'footprint', 'FM_RELEASE_AGE_MS': '0', 'FM_RELEASE_INTERVAL_MS': '0', 'FM_RELEASE_MAX_INTERVAL_MS': '0'}, str(root / 'libfastmalloc2.so'))}
with (dest / 'matrix.jsonl').open('a') as f:
    for rep in range(5):
        names = list(variants)
        names = names[rep:] + names[:rep]
        for app in ['python', 'node']:
            for name in names:
                extra, lib = variants[name]
                env = os.environ.copy()
                env.update(extra)
                if lib:
                    env['LD_PRELOAD'] = lib
                cmd = ['python3' if app == 'python' else 'node', f"bench/apps/workloads/churn.{('py' if app == 'python' else 'js')}", 'bench/apps/work/big.json']
                p = subprocess.run(['python3', 'bench/apps/run_timed.py', *cmd], env=env, text=True, capture_output=True)
                m = re.search('elapsed=([\\d.]+) maxrss_kb=(\\d+) exit=(\\d+)', p.stderr)
                row = dict(rep=rep, app=app, variant=name, stdout=p.stdout, stderr=p.stderr, rc=p.returncode)
                if m:
                    row.update(seconds=float(m[1]), rss_kb=int(m[2]))
                f.write(json.dumps(row) + '\n')
                f.flush()
                print(app, name, rep, m.groups() if m else p.stderr, flush=True)
                assert p.returncode == 0 and 'checksum' in p.stdout
