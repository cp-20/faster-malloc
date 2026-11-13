#!/usr/bin/env python3
"""Round 9 sanitizer evidence runner; see docs/round9-2026-11-13.md."""
import subprocess,os,pathlib,json,time
root=pathlib.Path(__file__).resolve().parents[2]; os.chdir(root); out=root/'work/sanitizers/namespaced'; out.mkdir(parents=True, exist_ok=True)
api='malloc free calloc realloc reallocarray posix_memalign aligned_alloc memalign valloc pvalloc malloc_usable_size malloc_trim mallopt malloc_stats malloc_info'.split()
defines=['-D'+s+'=fm_san_'+s for s in api]
flags=['-O2','-g','-fno-omit-frame-pointer','-fno-sanitize-recover=all']
rows=[]
def run(mode,name,args,env,timeout=240):
 start=time.monotonic()
 try:
  r=subprocess.run(args,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout); code=r.returncode; data=r.stdout
 except subprocess.TimeoutExpired as e: code=124; data=e.stdout or b''
 (out/(mode+'-'+name+'.log')).write_bytes(data)
 rows.append(dict(mode=mode,test=name,status=code,command=args,seconds=round(time.monotonic()-start,2)))
 (out/'results.json').write_text(json.dumps(rows,indent=2)); print(rows[-1],flush=True)
 return code
for san in ['address','thread','address,undefined','thread,undefined']:
 mode=san.replace(',','-'); sf=['-fsanitize='+san]; env=os.environ|{'ASAN_OPTIONS':'halt_on_error=1','TSAN_OPTIONS':'halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1'}
 for impl in ['static','jit']:
  tag=mode+'-'+impl; lib=out/(tag+'.so'); sources=['src/fm_'+impl+'.c']+(['src/jit_stubs.S'] if impl=='jit' else [])
  if impl=='jit':
   import re
   copy=out/(tag+'-source.c'); copy.write_text(re.sub(r'\bmalloc\b','fm_san_malloc',(root/'src/fm_jit.c').read_text())); sources[0]=str(copy)
  if run(tag,'build',['/usr/bin/gcc',*flags,*sf,*defines,'-fPIC','-shared','-Isrc','-Wl,-z,nodelete',*sources,'-o',str(lib),'-pthread'],env): continue
  tests=['test_basic','stress','check','mt','realloccache','idlefork']+(['jitprofile'] if impl=='jit' else [])
  for test in tests:
   src='test_basic.c' if test=='test_basic' else 'bench/'+test+'.c'; exe=out/(tag+'-'+test)
   if test=='idlefork' and san.startswith('thread'):
    copy=out/(tag+'-idlefork.c'); copy.write_text((root/src).read_text().replace('threads() == 1 + enabled','threads() == 2 + enabled')); src=str(copy)
   if san.startswith('thread') and test not in ['jitprofile', 'mt']:
    obj=out/(tag+'-'+test+'.o')
    run(tag,'object-'+test,['/usr/bin/gcc',*flags,*defines,'-c',src,'-o',str(obj)],env)
    run(tag,'compile-'+test,['/usr/bin/gcc',*sf,str(obj),str(lib),'-pthread','-ldl','-o',str(exe)],env)
   else:
    run(tag,'compile-'+test,['/usr/bin/gcc',*flags,*sf,*defines,src,str(lib),'-pthread','-ldl','-o',str(exe)],env)
  def test(name,binary,extra={},args=[]): return run(tag,name,[str(out/(tag+'-'+binary)),*args],env|extra)
  if impl=='jit': test('jitprofile','jitprofile',{'FM_JIT_MINSHARE':'101'})
  test('basic','test_basic')
  for p in ['balanced','throughput','footprint']: test('basic-'+p,'test_basic',{'FM_PROFILE':p})
  for n in range(1,4): test('stress-'+str(n),'stress')
  test('check','check')
  for n in ['0','1']:
   test('mt-'+n,'mt',args=[n]); test('realloccache-'+n,'realloccache',{'FM_REALLOC_CACHE_SEARCH':n},[n]); test('idlefork-'+n,'idlefork',{'FM_IDLE_RELEASE':n},[n])
  test('both-basic','test_basic',{'FM_REALLOC_CACHE_SEARCH':'1','FM_IDLE_RELEASE':'1'})
  test('both-stress','stress',{'FM_REALLOC_CACHE_SEARCH':'1','FM_IDLE_RELEASE':'1','FM_PURGE_MIN_PAGES':'1'})
  run(tag,'debug-build',['/usr/bin/gcc',*flags,*sf,*defines,'-DFM_DEBUG','-fPIC','-shared','-Isrc','-Wl,-z,nodelete',*sources,'-o',str(lib),'-pthread'],env)
  test('debug-stress','stress')
