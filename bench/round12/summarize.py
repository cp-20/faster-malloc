import json,statistics
from pathlib import Path
root=Path('bench/results/round12')
lines=[]
def table(title,rows,cases,builds,keys):
 lines.extend(['',title,'','| case / metric | '+' | '.join(builds)+' |','|---|'+'---|'*len(builds)])
 for case in cases:
  for key in keys(case):
   cells=[]
   for b in builds:
    v=[r[key] for r in rows if r.get('case','lua')==case and r['build']==b and not r.get('warmup',False)]
    cells.append(f'{statistics.median(v):,.3f} ({min(v):,.3f}–{max(v):,.3f})' if key=='seconds' else f'{statistics.median(v):,.0f} ({min(v):,}–{max(v):,})')
   lines.append('| '+case+' / '+key+' | '+' | '.join(cells)+' |')
table('App-matrix baseline; KiB and seconds',json.loads((root/'baseline.json').read_text()),['lua'],['glibc','v2','jemalloc','mimalloc'],lambda c:['rss','seconds'])
table('Mechanism comparison; KiB and seconds',json.loads((root/'comparison.json').read_text()),['lua'],['base','cold'],lambda c:['rss','seconds'])
rows=[json.loads(l) for l in (root/'guards.jsonl').read_text().splitlines()]
table('Guard workloads; KiB and seconds',rows,['bursty','consprobe','local','cross','many','census'],['base','cold'],lambda c:['rss','seconds'] if c in ('bursty','consprobe') else ['seconds'])
(root/'summary.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
confirm=[json.loads(l) for l in (root/'churn-confirm.jsonl').read_text().splitlines()]
assert len(confirm)==160
lines=[]
table('Ten-round confirmation; seconds',confirm,['local','cross','many','census'],['base','base-repeat','cold','cold-off'],lambda c:['seconds'])
table('All fifteen primary measured pairs; seconds',rows+confirm,['local','cross','many','census'],['base','cold'],lambda c:['seconds'])
(root/'confirmation.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
