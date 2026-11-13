"""Alternating same-host samples; no min-of-N selection."""
import json, os, platform, statistics, subprocess, time
from pathlib import Path
root = Path.cwd()
out = root / 'bench/results/round9-reclamation'
cases = {'local':['bench/mt','0','10000000','8'],
         'cross':['bench/mt','1','10000000','8'],
         'many':['work/reclamation/ownership','200000','0'],
         'census':['work/reclamation/ownership','200000','1'],
         'bursty':['work/reclamation/bursty']}
rows=[]
with (out/'samples.jsonl').open('w') as f:
    for case, cmd in cases.items():
        for rep in range(6):
            for build in (['locked','epoch'] if rep%2==0 else ['epoch','locked']):
                env = os.environ | {'LD_PRELOAD':str(root/f'work/reclamation/{build}.so')}
                t=time.monotonic()
                p=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=180)
                elapsed=time.monotonic()-t
                if p.returncode:
                    (out/"failure.json").write_text(json.dumps(dict(case=case,build=build,rep=rep,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr),indent=2))
                assert p.returncode == 0, (case,build,p.returncode,p.stderr)
                if case in ('many','census'):
                    assert p.stdout.strip()==f'ownership ok: 800000 transfers, trim={int(case=="census")}'
                if case=='bursty':
                    checksum=p.stdout.split('checksum=')[-1].strip()
                    if rows and any(r['case']=='bursty' for r in rows):
                        assert checksum==next(r['output'].split('checksum=')[-1].strip() for r in rows if r['case']=='bursty')
                r=dict(case=case,build=build,rep=rep,warmup=rep==0,seconds=elapsed,output=p.stdout)
                rows.append(r); f.write(json.dumps(r)+'\n'); f.flush()
        print(case,'complete',flush=True)
with (out/'summary.md').open('w') as f:
    f.write('| case | locked median (range), s | epoch median (range), s | delta |\n|---|---|---|---|\n')
    for case in cases:
        values={b:[r['seconds'] for r in rows if r['case']==case and r['build']==b and not r['warmup']] for b in ('locked','epoch')}
        cells=[f'{statistics.median(v):.6f} ({min(v):.6f}–{max(v):.6f})' for v in values.values()]
        delta=(statistics.median(values['epoch'])/statistics.median(values['locked'])-1)*100
        f.write(f'| {case} | {cells[0]} | {cells[1]} | {delta:+.2f}% |\n')
