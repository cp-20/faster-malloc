"""Alternating same-host samples; no min-of-N selection."""
import json, os, sys, statistics, subprocess, time
from pathlib import Path
root = Path.cwd()
candidate = sys.argv[1] if len(sys.argv) > 1 else 'stripe'
assert candidate in ('stripe', 'adaptive')
out = root / 'bench/results/round10-lock-cost'
if candidate == 'adaptive':
    out = out / 'adaptive'
out.mkdir(parents=True, exist_ok=True)
cases = {'local':['bench/mt','0','10000000','8'],
         'cross':['bench/mt','1','10000000','8'],
         'many':['work/lock-cost/ownership','200000','0'],
         'census':['work/lock-cost/ownership','200000','1']
}
rows=[]
with (out/'samples.jsonl').open('w') as f:
    for case, cmd in cases.items():
        for rep in range(6):
            for build in (['base',candidate] if rep%2==0 else [candidate,'base']):
                env = os.environ | {'LD_PRELOAD':str(root/f'work/lock-cost/{build}.so')}
                t=time.monotonic()
                p=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=180)
                elapsed=time.monotonic()-t
                if p.returncode:
                    (out/"failure.json").write_text(json.dumps(dict(case=case,build=build,rep=rep,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr),indent=2))
                assert p.returncode == 0, (case,build,p.returncode,p.stderr)
                if case in ('many','census'):
                    assert p.stdout.strip()==f'ownership ok: 800000 transfers, trim={int(case=="census")}'
                r=dict(case=case,build=build,rep=rep,warmup=rep==0,seconds=elapsed,output=p.stdout)
                rows.append(r); f.write(json.dumps(r)+'\n'); f.flush()
        print(case,'complete',flush=True)
with (out/'summary.md').open('w') as f:
    f.write(f'| case | base median (range), s | {candidate} median (range), s | delta |\n|---|---|---|---|\n')
    for case in cases:
        values={b:[r['seconds'] for r in rows if r['case']==case and r['build']==b and not r['warmup']] for b in ('base',candidate)}
        cells=[f'{statistics.median(v):.6f} ({min(v):.6f}–{max(v):.6f})' for v in values.values()]
        delta=(statistics.median(values[candidate])/statistics.median(values['base'])-1)*100
        f.write(f'| {case} | {cells[0]} | {cells[1]} | {delta:+.2f}% |\n')
summary = {}
for case in cases:
    entry = {}
    for build in ('base', candidate):
        v = [r['seconds'] for r in rows if r['case'] == case and r['build'] == build and not r['warmup']]
        entry[build] = dict(n=len(v), median=statistics.median(v), min=min(v), max=max(v),
                            cv=100 * statistics.stdev(v) / statistics.mean(v))
    pairs = {rep: {r['build']: r['seconds'] for r in rows
                   if r['case'] == case and r['rep'] == rep} for rep in range(1, 6)}
    entry['faster_pairs'] = sum(p[candidate] < p['base'] for p in pairs.values())
    summary[case] = entry
(out / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
