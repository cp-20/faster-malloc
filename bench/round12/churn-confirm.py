"""Predeclared follow-up: 10 rounds, four labels, no discarded samples.
base-repeat shares base's path and environment. cold-off shares cold's code.
Run after verification finishes: never alongside correctness or builds.
"""
import os,subprocess,time,json,statistics
from pathlib import Path
root=Path.cwd();rows=[]
cases={'local':['bench/mt','0','10000000','8'],'cross':['bench/mt','1','10000000','8'],'many':['work/round12/ownership','200000','0'],'census':['work/round12/ownership','200000','1']}
variants={'base':('base',1),'base-repeat':('base',1),'cold':('cold',1),'cold-off':('cold',0)}
with open('bench/results/round12/churn-confirm.jsonl','w') as f:
 for case,cmd in cases.items():
  for rep in range(10):
   order=list(variants);order=order[rep%4:]+order[:rep%4]
   if rep%2:order.reverse()
   for b in order:
    lib,mode=variants[b];env=os.environ|{'LD_PRELOAD':str(root/f'work/round12/{lib}.so'),'FM_LCACHE_COLD_PURGE':str(mode)}
    start=time.monotonic();p=subprocess.run(cmd,env=env,capture_output=True,text=True,timeout=180)
    row=dict(case=case,build=b,rep=rep,seconds=time.monotonic()-start,rc=p.returncode,stdout=p.stdout,stderr=p.stderr)
    assert p.returncode==0,row
    if case in ('many','census'):assert p.stdout.strip()==f'ownership ok: 800000 transfers, trim={int(case=="census")}'
    rows.append(row);f.write(json.dumps(row)+'\n');f.flush()
  print(case,[(b,statistics.median(v:=[r['seconds'] for r in rows if r['case']==case and r['build']==b]),min(v),max(v)) for b in variants],flush=True)
