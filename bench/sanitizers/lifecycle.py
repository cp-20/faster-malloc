#!/usr/bin/env python3
"""Round 9 sanitizer evidence runner; see docs/round9-2026-11-13.md."""
import pathlib,subprocess,os,json,time,sys
root=pathlib.Path(__file__).resolve().parents[2]; os.chdir(root); early_only='--early-only' in sys.argv; out=root/'work/sanitizers'/('early-recheck' if early_only else 'lifecycle'); out.mkdir(parents=True, exist_ok=True)
api='malloc free calloc realloc reallocarray posix_memalign aligned_alloc memalign valloc pvalloc malloc_usable_size malloc_trim mallopt malloc_stats malloc_info'.split()
defines=['-D'+s+'=fm_san_'+s for s in api]
rows=[]
def run(tag,name,args,env,timeout=90):
 start=time.monotonic()
 try:
  p=subprocess.run(args,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout); code=p.returncode; data=p.stdout
 except subprocess.TimeoutExpired as e: code=124; data=e.stdout or b''
 (out/(tag+'-'+name+'.log')).write_bytes(data); rows.append(dict(mode=tag,test=name,status=code,command=args,seconds=round(time.monotonic()-start,2)))
 (out/'results.json').write_text(json.dumps(rows,indent=2)); print(rows[-1],flush=True); return code
# Only symbol lookup strings and the sanitizer-forbidden DEEPBIND flag change.
s=(root/'bench/idlelifecycle.c').read_text().replace('"malloc"','"fm_san_malloc"').replace('"free"','"fm_san_free"').replace(' | RTLD_DEEPBIND','')
(out/'idlelifecycle.c').write_text(s)
for san in ['address','thread','address,undefined','thread,undefined']:
 mode=san.replace(',','-'); flags=['-O2','-g','-fno-omit-frame-pointer','-fno-sanitize-recover=all','-fsanitize='+san,*defines]
 env=os.environ|{'ASAN_OPTIONS':'halt_on_error=1','TSAN_OPTIONS':'halt_on_error=1','UBSAN_OPTIONS':'halt_on_error=1:print_stacktrace=1','GLIBC_TUNABLES':'glibc.rtld.optional_static_tls=131072'}
 for impl in ['static','jit']:
  tag=mode+'-'+impl; sources=['src/fm_'+impl+'.c']+(['src/jit_stubs.S'] if impl=='jit' else []); lib=out/(tag+'.so')
  if impl=='jit':
   import re
   copy=out/(tag+'-source.c'); copy.write_text(re.sub(r'\bmalloc\b','fm_san_malloc',(root/'src/fm_jit.c').read_text())); sources[0]=str(copy)
  def build(name,extra=[]): return run(tag,'build-'+name,['/usr/bin/gcc',*flags,'-fPIC','-shared','-Isrc','-Wl,-z,nodelete',*extra,*sources,'-o',str(lib),'-pthread'],env)
  build('normal')
  # Match the dependency-constructor ordering of earlyinit.sh.
  early=out/(tag+'-early.so'); main=out/(tag+'-early')
  run(tag,'compile-early-lib',['/usr/bin/gcc',*flags,'-fPIC','-shared','bench/early_lib.c','-o',str(early)],env)
  run(tag,'compile-early-main',['/usr/bin/gcc',*flags,'bench/early_main.c',str(early),str(lib),'-o',str(main)],env)
  runtime=subprocess.check_output(['/usr/bin/gcc','-print-file-name='+('libasan.so' if san.startswith('address') else 'libtsan.so')],text=True).strip()
  run(tag,'earlyinit',[str(main)],env|{'LD_PRELOAD':runtime+':'+str(lib)})
  if early_only: continue
  lifecycle_source=out/(tag+'-lifecycle.c'); lifecycle_source.write_text(s.replace('threads() == 1 + enabled','threads() == 2 + enabled') if san.startswith('thread') else s)
  for name,src in [('tlsexit','bench/tlsexit.c'),('idlefork','bench/idlefork.c'),('lifecycle',str(lifecycle_source))]:
   if name=='idlefork' and san.startswith('thread'):
    copy=out/(tag+'-idlefork.c'); copy.write_text((root/src).read_text().replace('threads() == 1 + enabled','threads() == 2 + enabled')); src=str(copy)
   run(tag,'compile-'+name,['/usr/bin/gcc',*flags,src,str(lib),'-pthread','-ldl','-o',str(out/(tag+'-'+name))],env)
  run(tag,'tlsexit',[str(out/(tag+'-tlsexit'))],env)
  for enabled in ['0','1']: run(tag,'lifecycle-'+enabled,[str(out/(tag+'-lifecycle')),str(lib),enabled],env|{'FM_IDLE_RELEASE':enabled})
  for fault,define,expected in [('atfork','-DFM_TEST_IDLE_ATFORK_FAIL','0'),('create','-DFM_TEST_IDLE_CREATE_FAILURES=-1','0'),('retry','-DFM_TEST_IDLE_CREATE_FAILURES=1','1')]:
   build(fault,[define])
   if fault!='atfork': run(tag,'idlefork-'+fault,[str(out/(tag+'-idlefork')),expected],env|{'FM_IDLE_RELEASE':'1'})
   run(tag,'lifecycle-'+fault,[str(out/(tag+'-lifecycle')),str(lib),expected],env|{'FM_IDLE_RELEASE':'1'})
