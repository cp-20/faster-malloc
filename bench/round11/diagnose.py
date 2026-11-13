"""Separate counter runs: instrumentation is excluded from throughput claims."""
import json, os, re, subprocess, sys, tempfile, time
from pathlib import Path
root=Path.cwd()
name=sys.argv[1] if len(sys.argv)>1 else 'diagnostics'
out=root/'bench/results/round11-crossfree'/f'{name}.jsonl'
with out.open('w') as f:
    for rep in range(6):
        for mode in (['blocking','trylock'] if rep%2==0 else ['trylock','blocking']):
            with tempfile.TemporaryFile(mode='w+') as log:
                start=time.monotonic()
                p=subprocess.Popen(['bench/mt','1','10000000','8'],env=os.environ|{'LD_PRELOAD':str(root/f'work/round11/{mode}-diag.so')},stdout=log,stderr=log,text=True)
                _,status,usage=os.wait4(p.pid,0)
                p.returncode=os.waitstatus_to_exitcode(status)
                elapsed=time.monotonic()-start
                log.seek(0);output=log.read()
            counts={k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',output)}
            row=dict(rep=rep,warmup=rep==0,build=mode,returncode=p.returncode,output=output,counts=counts,seconds=elapsed,user=usage.ru_utime,system=usage.ru_stime,peak_kib=usage.ru_maxrss,voluntary=usage.ru_nvcsw,involuntary=usage.ru_nivcsw)
            f.write(json.dumps(row)+'\n');f.flush()
            assert p.returncode==0,row
            assert counts['attempts']==counts['acquired']+counts['bail'],row
print(out.read_text())
