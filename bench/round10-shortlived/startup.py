import subprocess, os, json, re
from pathlib import Path
root = Path.cwd()
libs = {'glibc': '', 'v2': str(root / 'libfastmalloc2.so'), 'jemalloc': '/lib/x86_64-linux-gnu/libjemalloc.so.2', 'mimalloc': str(root / 'work/libmimalloc.so')}
with open('bench/results/round10-shortlived/startup.jsonl', 'w') as f:
    for rep in range(5):
        for app, cmd in [('python', ['python3', '-c', 'pass']), ('node', ['node', '-e', ''])]:
            for name, lib in libs.items():
                env = os.environ.copy()
                if lib:
                    env['LD_PRELOAD'] = lib
                p = subprocess.run(['python3', 'bench/apps/run_timed.py', *cmd], env=env, text=True, capture_output=True)
                m = re.search('elapsed=([\\d.]+) maxrss_kb=(\\d+)', p.stderr)
                row = dict(rep=rep, app=app, variant=name, seconds=float(m[1]), rss_kb=int(m[2]), rc=p.returncode)
                f.write(json.dumps(row) + '\n')
                assert p.returncode == 0
