import subprocess,os,json,statistics
from pathlib import Path
root=Path.cwd(); libs={'glibc':'','v2':str(root/'work/round12/base.so'),'jemalloc':'/lib/x86_64-linux-gnu/libjemalloc.so.2','mimalloc':str(root/'work/tools/usr/lib/x86_64-linux-gnu/libmimalloc.so.3')}
rows=[]
for rep in range(5):
 for name in list(libs)[rep%4:]+list(libs)[:rep%4]:
  env=os.environ.copy();env.pop('LD_PRELOAD',None)
  if libs[name]:env['LD_PRELOAD']=libs[name]
  p=subprocess.run(['python3','bench/apps/run_timed.py','work/tools/usr/bin/lua5.4','bench/apps/workloads/churn.lua'],env=env,capture_output=True,text=True)
  assert p.returncode==0 and '19466078' in p.stdout,(p.stdout,p.stderr)
  import re
  row=dict(rep=rep,build=name,seconds=float(re.search(r'elapsed=([\d.]+)',p.stderr)[1]),rss=int(re.search(r'maxrss_kb=(\d+)',p.stderr)[1]),stdout=p.stdout,stderr=p.stderr);rows.append(row)
Path('bench/results/round12/baseline.json').write_text(json.dumps(rows,indent=2))
for b in libs:
 print(b,[(k,statistics.median(v:=[r[k] for r in rows if r['build']==b]),min(v),max(v)) for k in ['rss','seconds']],flush=True)
