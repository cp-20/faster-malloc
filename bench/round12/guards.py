"""Five alternating pairs, one excluded warmup for throughput cases."""
import json,os,re,statistics,subprocess,time
from pathlib import Path
root=Path.cwd()
cases={'bursty':['bench/apps/bursty'],'consprobe':['bench/consprobe'],'local':['bench/mt','0','10000000','8'],'cross':['bench/mt','1','10000000','8'],'many':['work/round12/ownership','200000','0'],'census':['work/round12/ownership','200000','1']}
rows=[]
with open('bench/results/round12/guards.jsonl','w') as f:
 for case,cmd in cases.items():
  for rep in range(6):
   for b in (['base','cold'] if rep%2==0 else ['cold','base']):
    start=time.monotonic();p=subprocess.run(cmd,env=os.environ|{'LD_PRELOAD':str(root/f'work/round12/{b}.so'),'FM_LCACHE_COLD_PURGE':'1'},capture_output=True,text=True,timeout=180)
    row=dict(case=case,build=b,rep=rep,warmup=rep==0,seconds=time.monotonic()-start,rc=p.returncode,stdout=p.stdout,stderr=p.stderr)
    assert p.returncode==0,row
    if case in ('bursty','consprobe'):
     line=next(l for l in p.stdout.splitlines() if l.startswith('idle2' if case=='bursty' else 'p3 settled'))
     row['rss']=int(re.search(r'rss=(\d+)',line)[1])
    if case in ('many','census'):assert p.stdout.strip()==f'ownership ok: 800000 transfers, trim={int(case=="census")}'
    rows.append(row);f.write(json.dumps(row)+'\n');f.flush()
  print(case,[(b,k,statistics.median(v:=[r[k] for r in rows if r['case']==case and r['build']==b and not r['warmup']]),min(v),max(v)) for b in ('base','cold') for k in (['rss','seconds'] if case in ('bursty','consprobe') else ['seconds'])],flush=True)
