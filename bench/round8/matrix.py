"""Five matrix rounds; resume incomplete attempts without losing raw records."""
import subprocess, os, json, time
from pathlib import Path
root=Path.cwd()
e=os.environ.copy()
e.update(TIME_BIN=str(root/'work/tools/usr/bin/time'),PATH=str(root/'work/bin')+':'+str(root/'work/tools/usr/bin')+':'+e['PATH'],LD_LIBRARY_PATH=str(root/'work/tools/usr/lib/x86_64-linux-gnu'),RUBYLIB=str(root/'work/tools/usr/lib/ruby/3.3.0')+':'+str(root/'work/tools/usr/lib/x86_64-linux-gnu/ruby/3.3.0'),PGBIN=str(root/'work/tools/usr/lib/postgresql/17/bin'),MIMALLOC_LIB=str(root/'work/tools/usr/lib/x86_64-linux-gnu/libmimalloc.so.3'),TCMALLOC_LIB=str(root/'work/tools/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4'),REPS='1',PGREPS='1')
names=['glibc','v1','v2','jit','jemalloc','mimalloc','tcmalloc']

def valid(row):
    fields=row['stdout'].strip().splitlines()[-1].split()
    return (row['exit']==0 and len(fields)=={'nginx':6,'pgbench':5,'ruby':4}[row['app']]
            and not any(x in row['stdout'] for x in ['FAIL','MISSING','MISMATCH']))

def main():
    path=Path('bench/results/round8/matrix.jsonl')
    rows=[json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
    done={(r['rep'],r['app'],r['allocator']) for r in rows if valid(r)}
    exclusions=[]
    for index,row in enumerate(rows):
        if not valid(row):
            reason=('GNU time missing from base image' if '/usr/bin/time: No such file' in row['stderr'] else
                    'harness shell parse error after editing active script' if row['exit'] else
                    'nginx RSS file read empty during in-place sampler write')
            exclusions.append(dict(index=index,reason=reason,app=row['app'],allocator=row['allocator'],rep=row['rep']))
    Path('bench/results/round8/matrix-exclusions.json').write_text(json.dumps(exclusions,indent=2)+'\n')
    with path.open('a') as f:
        for rep in range(5):
            order=names[rep:]+names[:rep]
            if rep%2: order=order[::-1]
            for app in ['nginx','pgbench','ruby']:
                for name in order:
                    if (rep,app,name) in done:
                        continue
                    start=time.monotonic()
                    p=subprocess.run(['bash',f'bench/apps/{app}.sh'],env=e|{'CELL_ALLOCATORS':name},text=True,capture_output=True,timeout=180)
                    row=dict(rep=rep,app=app,allocator=name,exit=p.returncode,stdout=p.stdout,stderr=p.stderr,driver_seconds=time.monotonic()-start)
                    f.write(json.dumps(row)+'\n'); f.flush()
                    print(rep,app,name,p.returncode,p.stdout.strip().splitlines()[-1:],flush=True)
                    assert valid(row),row

if __name__=='__main__':
    main()
