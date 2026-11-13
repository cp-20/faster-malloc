"""One excluded warmup, five alternating pairs; retain every measured sample."""
import json, os, statistics, subprocess, sys, time
from pathlib import Path
root = Path.cwd()
control, candidate = sys.argv[1:3]
cases = {'local':['bench/mt','0','10000000','8'],
         'cross':['bench/mt','1','10000000','8'],
         'many':['work/round11/ownership','200000','0'],
         'census':['work/round11/ownership','200000','1']}
if len(sys.argv)>3:
    cases = {k:cases[k] for k in sys.argv[3:]}
out = root/'bench/results/round11-crossfree'/f'{control}-vs-{candidate}'
out.mkdir(parents=True,exist_ok=True)
rows=[]
with (out/'samples.jsonl').open('w') as f:
    for case, cmd in cases.items():
        for rep in range(6):
            for build in ([control,candidate] if rep%2==0 else [candidate,control]):
                start=time.monotonic()
                p=subprocess.run(cmd,env=os.environ|{'LD_PRELOAD':str(root/f'work/round11/{build}.so')},capture_output=True,text=True,timeout=180)
                row=dict(case=case,build=build,rep=rep,warmup=rep==0,seconds=time.monotonic()-start,returncode=p.returncode,stdout=p.stdout,stderr=p.stderr)
                rows.append(row);f.write(json.dumps(row)+'\n');f.flush()
                assert p.returncode==0,row
                if case in ('many','census'):
                    assert p.stdout.strip()==f'ownership ok: 800000 transfers, trim={int(case=="census")}'
        print(case,'complete',flush=True)
summary={}
lines=[f'| case | {control} median (range), s | {candidate} median (range), s | delta |','|---|---|---|---|']
for case in cases:
    s={}
    for b in (control,candidate):
        v=[r['seconds'] for r in rows if r['case']==case and r['build']==b and not r['warmup']]
        s[b]=dict(n=len(v),median=statistics.median(v),min=min(v),max=max(v))
    s['delta_percent']=100*(s[candidate]['median']/s[control]['median']-1)
    s['separated_win']=s[candidate]['max']<s[control]['min']
    s['faster_pairs']=sum(next(r['seconds'] for r in rows if r['case']==case and r['build']==candidate and r['rep']==i)<next(r['seconds'] for r in rows if r['case']==case and r['build']==control and r['rep']==i) for i in range(1,6))
    summary[case]=s
    cells=[f"{s[b]['median']:.6f} ({s[b]['min']:.6f}–{s[b]['max']:.6f})" for b in (control,candidate)]
    lines.append(f"| {case} | {cells[0]} | {cells[1]} | {s['delta_percent']:+.2f}% |")
(out/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
(out/'summary.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines),flush=True)
