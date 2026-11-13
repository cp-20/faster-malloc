#!/usr/bin/env python3
"""Round 9 sanitizer evidence runner; see docs/round9-2026-11-13.md."""
import os, subprocess, pathlib, json, time
root=pathlib.Path(__file__).resolve().parents[2]; os.chdir(root); out=root/'work/sanitizers'
base='-g -fno-omit-frame-pointer -fno-sanitize-recover=all'
out.mkdir(parents=True, exist_ok=True)
(out/'bin').mkdir(exist_ok=True)
wrapper=out/'bin/cc'
wrapper.write_text('#!/bin/bash\nexec /usr/bin/gcc $SAN_FLAGS "$@"\n')
wrapper.chmod(0o755)
link=out/'bin/gcc'
if not link.exists(): link.symlink_to('cc')
results=[]
def run(mode,name,args,env,timeout=240):
    start=time.monotonic()
    try:
        p=subprocess.run(args,env=env,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout)
        code=p.returncode; data=p.stdout
    except subprocess.TimeoutExpired as e:
        code=124; data=e.stdout or b''
    (out/f'{mode}-{name}.log').write_bytes(data)
    row=dict(mode=mode,test=name,status=code,command=args,seconds=round(time.monotonic()-start,2))
    results.append(row); (out/'results.json').write_text(json.dumps(results,indent=2)); print(row,flush=True)
    return code
for mode,san in [('undefined','undefined'),('address','address'),('thread','thread'),('address-undefined','address,undefined'),('thread-undefined','thread,undefined')]:
    env=os.environ.copy(); env.update(PATH=str(out/'bin')+':'+env['PATH'],CC=str(out/'bin/cc'),SAN_FLAGS=base+' -fsanitize='+san,UBSAN_OPTIONS='print_stacktrace=1:halt_on_error=1',ASAN_OPTIONS='halt_on_error=1',TSAN_OPTIONS='halt_on_error=1')
    subprocess.run(['make','clean'],env=env,stdout=subprocess.DEVNULL)
    if run(mode,'build',['make','-j4','CFLAGS=-O2 -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc'],env): continue
    # The exact established script, then each case independently so its fail-fast
    # policy cannot conceal later failures.
    battery = run(mode,'round7',['bash','bench/round7.sh'],env,900)
    for lib in ([] if mode=='undefined' and battery==0 else ['libfastmalloc2.so','libjitmalloc.so']):
        e=env|{'LD_PRELOAD':str(root/lib)}; tag='v2' if '2.' in lib else 'jit'
        cases=[('basic', ['./test_basic'],{})]+[(f'basic-{p}',['./test_basic'],{'FM_PROFILE':p}) for p in ['balanced','throughput','footprint']]
        cases += [(f'stress-{n}',['./bench/stress'],{}) for n in range(1,4)]
        cases += [('check',['./bench/check'],{}),('mt-local',['./bench/mt','0'],{}),('mt-cross',['./bench/mt','1'],{})]
        for n in ['0','1']:
            cases += [('realloccache-'+n,['./bench/realloccache',n],{'FM_REALLOC_CACHE_SEARCH':n}),('idlefork-'+n,['./bench/idlefork',n],{'FM_IDLE_RELEASE':n})]
        cases += [('both-basic',['./test_basic'],{'FM_REALLOC_CACHE_SEARCH':'1','FM_IDLE_RELEASE':'1'}),('both-stress',['./bench/stress'],{'FM_REALLOC_CACHE_SEARCH':'1','FM_IDLE_RELEASE':'1','FM_PURGE_MIN_PAGES':'1'})]
        for name,args,extra in cases: run(mode,tag+'-'+name,args,e|extra)
    for script in ([] if mode=='undefined' and battery==0 else ['earlyinit','tlsexit','idlelifecycle']): run(mode,script,['bash','bench/'+script+'.sh'],env)
    if mode!='undefined':
        run(mode,'jitprofile',['./bench/jitprofile'],env|{'LD_PRELOAD':str(root/'libjitmalloc.so'),'FM_JIT_MINSHARE':'101'})
    # Debug invariants in the same instrumented allocator builds.
    run(mode,'debug-build',['make','-B','-j4','libfastmalloc2.so','libjitmalloc.so','CFLAGS=-O2 -fPIC -fno-plt -Wall -Wextra -Wno-unused-parameter -Isrc -DFM_DEBUG'],env)
    for lib in ['libfastmalloc2.so','libjitmalloc.so']:
        run(mode,lib+'-debug-stress',['./bench/stress'],env|{'LD_PRELOAD':str(root/lib)})
