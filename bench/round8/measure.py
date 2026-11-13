import os,subprocess,time,json,argparse
from pathlib import Path
root=Path.cwd()
env=os.environ.copy()
env['LD_LIBRARY_PATH']=str(root/'work/tools/usr/lib/x86_64-linux-gnu')
env['RUBYLIB']=str(root/'work/tools/usr/lib/ruby/3.3.0')+':'+str(root/'work/tools/usr/lib/x86_64-linux-gnu/ruby/3.3.0')
apps={'lua':[str(root/'work/tools/usr/bin/lua5.4'),'bench/apps/workloads/churn.lua'], 'jq':['jq','[.[]|select(.v%3==0)|{id,t:(.v*.price)}]|sort_by(.t)|length','work/big.json'], 'ruby':[str(root/'work/tools/usr/bin/ruby3.3'),'--disable-gems','bench/apps/bench.rb']}
apps['python']=['python3','bench/apps/workloads/churn.py','work/big.json']
expected={'python':'python-churn checksum 625726','lua':'lua-churn checksum\t19466078','jq':'66395','ruby':32103}
def run(cmd,lib,extra={}):
 e=env|{'LD_PRELOAD':str(root/lib)}|extra
 # time only the target; wait4 provides this child's RSS, not cumulative max.
 start=time.monotonic(); p=subprocess.Popen(cmd,env=e,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 out=p.stdout.read(); err=p.stderr.read(); _,status,usage=os.wait4(p.pid,0);p.returncode=os.waitstatus_to_exitcode(status)
 assert p.returncode==0,(cmd,p.returncode,err.decode())
 return time.monotonic()-start,usage.ru_maxrss,out.decode().strip()
parser=argparse.ArgumentParser()
parser.add_argument('--apps', nargs='+', default=list(apps))
parser.add_argument('--append', action='store_true')
parser.add_argument('--skip-ownership', action='store_true')
args=parser.parse_args()
with open('bench/results/round8/realloc.jsonl','a' if args.append else 'w') as f:
 for app in args.apps:
  cmd=apps[app]
  for build in ['v2','jit']:
   lib='libfastmalloc2.so' if build=='v2' else 'libjitmalloc.so'
   for rep in range(10):
    for mode in ([0,1] if rep%2==0 else [1,0]):
     elapsed,rss,out=run(cmd,lib,{'FM_REALLOC_CACHE_SEARCH':str(mode)})
     if app=='ruby': assert json.loads(out)['checksum']==expected[app],out
     else: assert out==expected[app],out
     row=dict(app=app,build=build,mode=mode,rep=rep,warmup=rep==0,seconds=elapsed,rss_kib=rss,output=out)
     f.write(json.dumps(row)+'\n'); f.flush()
   print(app,build,'done',flush=True)
if not args.skip_ownership:
 with open('bench/results/round8/ownership.jsonl','w') as f:
  cases={'local':['bench/mt','0','10000000','8'],'cross':['bench/mt','1','10000000','8'],'many':['work/ownership','200000','0'],'census':['work/ownership','200000','1']}
  for case,cmd in cases.items():
   for rep in range(10):
    for build in (['round6','round7'] if rep%2==0 else ['round7','round6']):
     lib='work/round6/libfastmalloc2.so' if build=='round6' else 'libfastmalloc2.so'
     elapsed,rss,out=run(cmd,lib)
     row=dict(case=case,build=build,rep=rep,warmup=rep==0,seconds=elapsed,rss_kib=rss,output=out)
     f.write(json.dumps(row)+'\n');f.flush()
   print(case,'done',flush=True)
