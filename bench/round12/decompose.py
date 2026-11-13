import os, signal, struct, subprocess, time, json, sys
from pathlib import Path
root = Path.cwd()
lib = str(root / (sys.argv[1] if len(sys.argv)>1 else 'work/libdiag.so'))
syms = {line.split()[2]: int(line.split()[0], 16) for line in subprocess.check_output(['nm', '-D', lib], text=True).splitlines() if 'diag_small_live' in line or 'diag_large_live' in line}

def snapshot(pid):
    maps = Path(f'/proc/{pid}/maps').read_text().splitlines()
    mem = os.open(f'/proc/{pid}/mem', os.O_RDONLY)
    pm = os.open(f'/proc/{pid}/pagemap', os.O_RDONLY)

    def read(addr, n):
        try:
            return os.pread(mem, n, addr)
        except OSError:
            return b''

    def resident(addr, n):
        data = os.pread(pm, 8 * ((n + 4095) // 4096), addr // 4096 * 8)
        return sum((v[0] >> 63 for v in struct.iter_unpack('Q', data))) * 4
    counts = dict(small_resident_kb=0, large_live_resident_kb=0, cached_large_resident_kb=0, small_spans=0, fully_free_small_resident_kb=0, partial_small_resident_kb=0, small_live_capacity_kb=0)
    seen = set()
    for line in maps:
        fields = line.split()
        a, b = [int(x, 16) for x in fields[0].split('-')]
        if len(fields) > 5 and fields[-1] == lib and (fields[2] == '00000000'):
            for name, off in syms.items():
                counts[name + '_kb'] = struct.unpack('q', read(a + off, 8))[0] / 1024
        if fields[1][:2] != 'rw' or len(fields) > 5 or b - a > 2 ** 31:
            continue
        for addr in range((a + 65535) // 65536 * 65536, b, 65536):
            if not resident(addr, 4096):
                continue
            data = read(addr, 72)
            if len(data) != 72:
                continue
            cls, magic, nblocks, dead, base, size, user, align, lnext = struct.unpack('IIIIQQQQQ', data[:56])
            if magic != 1517177946 or addr in seen:
                continue
            seen.add(addr)
            if cls == 4294967295:
                key = 'cached_large_resident_kb' if user == 0 else 'large_live_resident_kb'
            else:
                key = 'small_resident_kb'
                counts['small_spans'] += 1
                counts['small_live_capacity_kb'] += user / 1024
                counts['partial_small_resident_kb' if user else 'fully_free_small_resident_kb'] += resident(base, size)
            counts[key] += resident(base, size)
    smaps = Path(f'/proc/{pid}/smaps_rollup').read_text()
    import re
    counts['rss_kb'] = int(re.search('^Rss:\\s+(\\d+)', smaps, re.M)[1])
    counts['other_resident_kb'] = counts['rss_kb'] - sum((counts[k] for k in ['small_resident_kb', 'large_live_resident_kb', 'cached_large_resident_kb']))
    baseline = 0
    named = False
    for line in Path(f'/proc/{pid}/smaps').read_text().splitlines():
        if re.match('^[0-9a-f]+-[0-9a-f]+ ', line):
            fields = line.split()
            named = len(fields) > 5 and (fields[5].startswith('/') or fields[5] in {'[stack]', '[vdso]', '[vvar]', '[vvar_vclock]', '[vsyscall]'})
        elif line.startswith('Rss:') and named:
            baseline += int(line.split()[1])
    counts['baseline_mappings_resident_kb'] = baseline
    counts['other_anonymous_resident_kb'] = counts['other_resident_kb'] - baseline
    os.close(mem)
    os.close(pm)
    return counts

rows=[]
for rep in range(3):
 env=os.environ.copy();env['LD_PRELOAD']=lib
 p=subprocess.Popen(['work/tools/usr/bin/lua5.4','bench/apps/workloads/churn.lua'],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 best={}
 while p.poll() is None:
  try:
   os.kill(p.pid,signal.SIGSTOP);os.waitpid(p.pid,os.WUNTRACED)
   s=snapshot(p.pid)
   if s['rss_kb']>best.get('rss_kb',0):best=s
  except (FileNotFoundError,ProcessLookupError):pass
  finally:
   try:os.kill(p.pid,signal.SIGCONT)
   except ProcessLookupError:pass
  time.sleep(.01)
 stdout,stderr=p.communicate();assert p.returncode==0,(stdout,stderr)
 rows.append(dict(rep=rep,peak=best,stdout=stdout.decode(),stderr=stderr.decode()));print(rows[-1],flush=True)
Path(sys.argv[2] if len(sys.argv)>2 else 'bench/results/round12/decomposition.json').write_text(json.dumps(rows,indent=2))
