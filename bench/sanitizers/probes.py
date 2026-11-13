#!/usr/bin/env python3
"""Reproduce runtime ordering controls and ADR 0044 before/after evidence."""
import json
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
os.chdir(root)
out = root / 'work/sanitizers/probes'
out.mkdir(parents=True, exist_ok=True)
rows = []

def run(name, args, extra=None):
    try:
        p = subprocess.run(args, env=os.environ | (extra or {}),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        status, data = p.returncode, p.stdout
    except subprocess.TimeoutExpired as e:
        status, data = 124, e.stdout or b''
    (out / (name + '.log')).write_bytes(data)
    rows.append(dict(test=name, command=args, environment=extra or {}, status=status))
    (out / 'results.json').write_text(json.dumps(rows, indent=2))
    print(name, status, flush=True)

(out / 'owner.c').write_text('''#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
int main(void) { Dl_info info; void *f=dlsym(RTLD_DEFAULT,"malloc");
 if (!dladdr(f,&info)) return 2; puts(info.dli_fname);
 void *p=malloc(32); free(p); return 0; }
''')
run('owner-build', ['/usr/bin/gcc', '-O2', '-g', str(out/'owner.c'), '-ldl', '-o', str(out/'owner')])
for san in ['address', 'thread']:
    exe = out / (san+'-plain')
    run(san+'-plain-build', ['/usr/bin/gcc', '-O2', '-g', '-fsanitize='+san, str(out/'owner.c'), '-ldl', '-o', str(exe)])
    run(san+'-plain', [str(exe)])
    lib = out / ('v2-'+san+'.so')
    run(san+'-allocator-build', ['/usr/bin/gcc', '-O2', '-g', '-fPIC', '-shared', '-Isrc', '-fsanitize='+san, '-fno-omit-frame-pointer', '-Wl,-z,nodelete', 'src/fm_static.c', '-o', str(lib)])
    runtime = subprocess.check_output(['/usr/bin/gcc', '-print-file-name=lib'+('a' if san=='address' else 't')+'san.so'], text=True).strip()
    run(san+'-allocator-first', [str(out/'owner')], {'LD_PRELOAD':str(lib)})
    run(san+'-runtime-first', [str(out/'owner')], {'LD_PRELOAD':runtime+':'+str(lib)})
    if san == 'address':
        run('address-order-disabled', [str(out/'owner')], {'LD_PRELOAD':str(lib), 'ASAN_OPTIONS':'verify_asan_link_order=0'})
run('incompatible', ['/usr/bin/gcc', '-fsanitize=address,thread', '-c', str(out/'owner.c'), '-o', str(out/'incompatible.o')])
(out/'posix.c').write_text('''#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
int main(void) { int (*volatile call)(void **,size_t,size_t)=posix_memalign;
 void *p=(void *)(uintptr_t)1; int e=call(&p,3,100);
 printf("alignment=3 errno=%d output=%p expected=0x1\\n",e,p);
 return e!=EINVAL || p!=(void *)(uintptr_t)1; }
''')
run('posix-build', ['/usr/bin/gcc', '-O2', str(out/'posix.c'), '-o', str(out/'posix')])
for impl in ['static', 'jit']:
    before = out / ('before-'+impl+'.c')
    before.write_bytes(subprocess.check_output(['git', 'show', 'd9c4364:src/fm_'+impl+'.c']))
    for label, source in [('before', before), ('after', Path('src/fm_'+impl+'.c'))]:
        lib = out / (label+'-'+impl+'.so')
        run(label+'-'+impl+'-build', ['/usr/bin/gcc', '-O2', '-g', '-fPIC', '-shared', '-Isrc', '-Wl,-z,nodelete', str(source), *(['src/jit_stubs.S'] if impl=='jit' else []), '-o', str(lib)])
        run(label+'-'+impl+'-posix', [str(out/'posix')], {'LD_PRELOAD':str(lib)})

(out/"threads.c").write_text('#define _GNU_SOURCE\n#include <dirent.h>\n#include <stdio.h>\n#include <pthread.h>\n#include <unistd.h>\nstatic int count(void) { DIR*d=opendir("/proc/self/task"); struct dirent*e; int n=0; while((e=readdir(d))) if(e->d_name[0]!=\'.\')n++;closedir(d);return n; }\nstatic void *worker(void*p){return p;}\nint main(void){printf("before=%d\\n",count());pthread_t t;pthread_create(&t,0,worker,0);pthread_join(t,0);usleep(100000);printf("after_join=%d\\n",count());return 0;}\n')
run('threads-build', ['/usr/bin/gcc', '-O2', '-g', '-fsanitize=thread', str(out/'threads.c'), '-pthread', '-o', str(out/'threads')])
run('threads', [str(out/'threads')])

# Fully instrumented reproducer for both JIT profiling races, before and after.
import re
api = 'malloc free calloc realloc reallocarray posix_memalign aligned_alloc memalign valloc pvalloc malloc_usable_size malloc_trim mallopt malloc_stats malloc_info'.split()
defines = ['-D'+name+'=fm_san_'+name for name in api]
for label in ['before', 'after']:
    original = (out/'before-jit.c').read_text() if label=='before' else Path('src/fm_jit.c').read_text()
    source = out/(label+'-jit-namespaced.c')
    source.write_text(re.sub(r'\bmalloc\b', 'fm_san_malloc', original))
    lib = out/(label+'-jit-tsan.so')
    flags = ['-O2', '-g', '-fno-omit-frame-pointer', '-fsanitize=thread', *defines]
    run(label+'-jit-tsan-build', ['/usr/bin/gcc', *flags, '-fPIC', '-shared', '-Isrc', '-Wl,-z,nodelete', str(source), 'src/jit_stubs.S', '-pthread', '-o', str(lib)])
    exe = out/(label+'-jitprofile')
    run(label+'-jitprofile-build', ['/usr/bin/gcc', *flags, 'bench/jitprofile.c', str(lib), '-pthread', '-o', str(exe)])
    run(label+'-jitprofile', [str(exe)], {'FM_JIT_MINSHARE':'101', 'FM_JIT_DEBUG':'1', 'TSAN_OPTIONS':'halt_on_error=0'})
