#!/usr/bin/env python3
"""Recheck cross-free with the test handoff atomics visible to TSan."""
from pathlib import Path
import os,subprocess,json,re,time
root=Path(__file__).resolve().parents[2];os.chdir(root);out=root/'work/sanitizers/mt-recheck';out.mkdir(parents=True, exist_ok=True)
api='malloc free calloc realloc reallocarray posix_memalign aligned_alloc memalign valloc pvalloc malloc_usable_size malloc_trim mallopt malloc_stats malloc_info'.split()
defines=['-D'+a+'=fm_san_'+a for a in api];rows=[]
def run(mode,name,args):
 start=time.monotonic();p=subprocess.run(args,env=os.environ|{'TSAN_OPTIONS':'halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'},stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=90)
 (out/(mode+'-'+name+'.log')).write_bytes(p.stdout);rows.append(dict(mode=mode,test=name,status=p.returncode,command=args,seconds=round(time.monotonic()-start,2)));(out/'results.json').write_text(json.dumps(rows,indent=2));print(rows[-1],flush=True)
for san in ['thread','thread,undefined']:
 for impl in ['static','jit']:
  mode=san.replace(',','-')+'-'+impl;flags=['-O2','-g','-fno-omit-frame-pointer','-fno-sanitize-recover=all','-fsanitize='+san,*defines];lib=out/(mode+'.so');sources=['src/fm_'+impl+'.c']+(['src/jit_stubs.S'] if impl=='jit' else [])
  if impl=='jit':
   src=out/(mode+'.c');src.write_text(re.sub(r'\bmalloc\b','fm_san_malloc',Path('src/fm_jit.c').read_text()));sources[0]=str(src)
  run(mode,'build',['/usr/bin/gcc',*flags,'-fPIC','-shared','-Isrc','-Wl,-z,nodelete',*sources,'-pthread','-o',str(lib)])
  exe=out/(mode+'-mt');run(mode,'compile-mt',['/usr/bin/gcc',*flags,'bench/mt.c',str(lib),'-pthread','-o',str(exe)])
  for rep in range(1,4):
   for arg in ['0','1']:run(mode,'mt-'+arg+'-'+str(rep),[str(exe),arg])
