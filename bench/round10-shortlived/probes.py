import subprocess, os, json, re
from pathlib import Path
root = Path.cwd()
with open('bench/results/round10-shortlived/probes.jsonl', 'w') as f:
    for rep in range(5):
        for profile in ['balanced', 'shortlived'] if rep % 2 == 0 else ['shortlived', 'balanced']:
            for app in ['bursty', 'consprobe']:
                env = os.environ.copy()
                env.update(LD_PRELOAD=str(root / 'libfastmalloc2.so'), FM_PROFILE=profile)
                p = subprocess.run(['bench/apps/bursty' if app == 'bursty' else 'bench/consprobe'], env=env, text=True, capture_output=True)
                row = dict(rep=rep, profile=profile, app=app, rc=p.returncode, stdout=p.stdout, stderr=p.stderr)
                line = next((l for l in p.stdout.splitlines() if l.startswith('idle2' if app == 'bursty' else 'p3 settled')))
                row['settled_kb'] = int(re.search('rss=(\\d+)', line)[1])
                f.write(json.dumps(row) + '\n')
                f.flush()
                print(app, profile, rep, row['settled_kb'], flush=True)
                assert p.returncode == 0
