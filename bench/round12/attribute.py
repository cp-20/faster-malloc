"""Pause actual startup processes; classify resident allocator mappings.

Uses the round-10 span-header/pagemap probe without allocator instrumentation.
Smaps totals are a synchronized snapshot, not additive pieces of historical HWM.
"""
import json
import os
from pathlib import Path
import re
import signal
import struct
import subprocess
import sys

PAGE = 4096
SPAN = 65536
MAGIC = 0x5A6E4C5A

def snapshot(pid, lib):
    counts = dict(small_spans=0, small_mapped_kb=0, small_resident_kb=0,
                  small_capacity_kb=0, large_mapped_kb=0, large_resident_kb=0, small_live_capacity_kb=0,
                  allocator_file_resident_kb=0, executable_anon_kb=0,
                  other_named_kb=0, other_anonymous_kb=0)
    classes = {}
    if 'diag' in lib:
        counts.update(cached_large_resident_kb=0, live_large_resident_kb=0)
    mem = os.open(f'/proc/{pid}/mem', os.O_RDONLY)
    pm = os.open(f'/proc/{pid}/pagemap', os.O_RDONLY)
    def read(addr, size):
        try:
            return os.pread(mem, size, addr)
        except OSError:
            return b''
    def rss(addr, size):
        data = os.pread(pm, 8 * ((size + PAGE - 1) // PAGE), addr // PAGE * 8)
        return 4 * sum(x[0] >> 63 for x in struct.iter_unpack('Q', data))
    for line in Path(f'/proc/{pid}/maps').read_text().splitlines():
        fields = line.split()
        lo, hi = (int(x, 16) for x in fields[0].split('-'))
        if fields[1][:2] != 'rw' or len(fields) > 5 or hi-lo > 2**31:
            continue
        for addr in range((lo + SPAN - 1) // SPAN * SPAN, hi, SPAN):
            if not rss(addr, PAGE):
                continue
            h = read(addr, 64)
            if len(h) != 64:
                continue
            cls, magic, nb, dead, base, size = struct.unpack('IIIIQQ', h[:32])
            if magic != MAGIC:
                continue
            # Aligned allocations have a different magic; included in residual.
            if cls == 0xffffffff:
                counts['large_mapped_kb'] += size // 1024
                counts['large_resident_kb'] += rss(base, size)
                if 'diag' in lib:
                    user = struct.unpack('Q', h[32:40])[0]
                    counts['live_large_resident_kb' if user else 'cached_large_resident_kb'] += rss(base, size)
            else:
                assert cls < 129 and size <= SPAN and size % PAGE == 0
                counts['small_spans'] += 1
                if 'diag' in lib:
                    counts['small_live_capacity_kb'] += struct.unpack('Q', h[32:40])[0] / 1024
                counts['small_mapped_kb'] += size // 1024
                counts['small_resident_kb'] += rss(base, size)
                # Class sizes come from the same generated header as the build.
                text = Path('src/size_classes2.h').read_text()
                sizes = list(map(int, re.search(r'kClassSize\[.*?\] = \{(.*?)\}', text, re.S)[1].replace('\n','').strip().strip(',').split(',')))
                counts['small_capacity_kb'] += nb * sizes[cls] / 1024
                c = classes.setdefault(str(cls), dict(size=sizes[cls], spans=0, mapped_kb=0, resident_kb=0))
                c['spans'] += 1
                c['mapped_kb'] += size // 1024
                c['resident_kb'] += rss(base, size)
    if lib:
        # ELF symbol offsets are relative to this DSO's zero-offset load mapping.
        load = next(int(line.split('-')[0], 16) for line in Path(f'/proc/{pid}/maps').read_text().splitlines()
                    if line.split()[-1] == lib and int(line.split()[2], 16) == 0)
        for line in subprocess.check_output(['nm', '-S', lib], text=True).splitlines():
            parts = line.split()
            if len(parts) != 4:
                continue
            offset, size, kind, name = parts
            if name in {'g_exact', 'fm_central'}:
                addr = load + int(offset, 16)
                start = addr // PAGE * PAGE
                size = (addr + int(size, 16) + PAGE - 1) // PAGE * PAGE - start
                counts[name + '_resident_pages_kb'] = rss(start, size)
            if name in {'diag_batch_pages', 'diag_census_calls'}:
                counts[name] = struct.unpack('Q', read(load + int(offset, 16), 8))[0]
    category = None
    total = 0
    for line in Path(f'/proc/{pid}/smaps').read_text().splitlines():
        if re.match(r'^[0-9a-f]+-[0-9a-f]+ ', line):
            fields = line.split()
            category = ('allocator_file_resident_kb' if lib and len(fields)>5 and fields[-1] == lib
                        else 'other_named_kb' if len(fields)>5
                        else 'executable_anon_kb' if 'x' in fields[1]
                        else 'other_anonymous_kb')
        elif line.startswith('Rss:'):
            n = int(line.split()[1])
            counts[category] += n
            total += n
    counts['other_anonymous_kb'] -= counts['small_resident_kb'] + counts['large_resident_kb']
    assert counts['other_anonymous_kb'] >= 0, counts
    counts['rss_kb'] = total
    counts['hwm_kb'] = int(re.search(r'VmHWM:\s+(\d+)', Path(f'/proc/{pid}/status').read_text())[1])
    if 'diag' not in lib:
        del counts['small_live_capacity_kb']
    counts['threads'] = len(list(Path(f'/proc/{pid}/task').iterdir()))
    counts['classes'] = classes
    os.close(mem)
    os.close(pm)
    return counts

def main():
    variants = {'glibc':'', 'base-v2':'work/round12/base/libfastmalloc2.so',
                'base-jit':'work/round12/base/libjitmalloc.so',
                'v2':'libfastmalloc2.so', 'jit':'libjitmalloc.so'}
    if '--diag' in sys.argv:
        variants = {'diag-base-v2':'work/round12/base-diag.so', 'diag-v2':'work/round12/diag.so'}
    with open(sys.argv[1], 'w') as f:
        for rep in range(3):
            for app, cmd in [('python', ['python3', '-c', 'import os; os.kill(os.getpid(),19)']),
                             ('node', ['node', '-e', 'process.kill(process.pid,"SIGSTOP")'])]:
                for name, lib in variants.items():
                    lib = str(Path(lib).resolve()) if lib else ''
                    env = {k:v for k,v in os.environ.items() if k != 'LD_PRELOAD' and not k.startswith('FM_')}
                    env['LD_PRELOAD'] = lib
                    p = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    try:
                        _, status = os.waitpid(p.pid, os.WUNTRACED)
                        assert os.WIFSTOPPED(status), (name, status)
                        row = dict(rep=rep, app=app, variant=name, **snapshot(p.pid, lib))
                        f.write(json.dumps(row)+'\n')
                        f.flush()
                    finally:
                        os.kill(p.pid, signal.SIGCONT)
                        out, err = p.communicate(timeout=10)
                    assert p.returncode == 0, err

if __name__ == '__main__':
    main()
