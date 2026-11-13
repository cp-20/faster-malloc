#!/usr/bin/env python3
"""Offline GNU sort / XZ matrix: medians, ranges and every sample in JSON.

Usage: MIMALLOC_LIB=/path/libmimalloc.so.2 python3 bench/apps/data_tools.py
Requires sort, xz, python3 and all four allocator libraries; fails closed.
"""
import hashlib
import json
import os
from pathlib import Path
import random
import re
import statistics
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
REPS = int(os.environ.get('REPS', '5'))
if REPS < 5:
    raise SystemExit('REPS must be at least 5')
LIBS = {
    'glibc': '',
    'v2': str(ROOT / 'libfastmalloc2.so'),
    'jemalloc': os.environ.get('JEMALLOC_LIB', '/lib/x86_64-linux-gnu/libjemalloc.so.2'),
    'mimalloc': os.environ.get('MIMALLOC_LIB', '/lib/x86_64-linux-gnu/libmimalloc.so.2'),
}
for name, lib in LIBS.items():
    if lib and not Path(lib).is_file():
        raise SystemExit(f'{name}: missing {lib}')


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    work = ROOT / 'bench/apps/work'
    work.mkdir(exist_ok=True)
    env = os.environ.copy()
    # Keep harness/checkers on libc and do not inherit allocator tuning.
    for key in list(env):
        if key == 'LD_PRELOAD' or key.startswith(('FM_', 'MALLOC_', 'MIMALLOC_')):
            del env[key]
    env['LC_ALL'] = 'C'
    with tempfile.TemporaryDirectory(prefix='data-tools-', dir=work) as tmp:
        tmp = Path(tmp)
        source = tmp / 'events.tsv'
        rng = random.Random(11013)
        with source.open('w') as stream:
            for i in range(400000):
                stream.write(f'{rng.randrange(1000000000):09d}\t{i}\tuser={rng.randrange(20000)} '
                             f'route=/api/items/{i % 971} status={200 + i % 5} '
                             f'payload={rng.getrandbits(128):032x}\n')
        commands = {
            'sort': ['sort', '--parallel=1', '-S', '128M', '-k1,1n', str(source)],
            'xz': ['xz', '-T1', '-6', '-c', str(source)],
        }
        samples = []
        hashes = {}
        for app, command in commands.items():
            output = tmp / 'output'
            # Untimed libc reference also warms input cache; validate semantics.
            with output.open('wb') as stream:
                subprocess.run(command, env=env, stdout=stream, check=True, timeout=600)
            hashes[app] = digest(output)
            if app == 'sort':
                subprocess.run(['sort', '-c', '-k1,1n', str(output)], env=env, check=True)
            else:
                decoded = tmp / 'decoded'
                with decoded.open('wb') as stream:
                    subprocess.run(['xz', '-dc', str(output)], env=env, stdout=stream, check=True)
                assert digest(decoded) == digest(source), 'XZ round-trip mismatch'
            names = list(LIBS)
            for rep in range(REPS):
                # Rotate first allocator each round to spread order/cache effects.
                for name in names[rep % 4:] + names[:rep % 4]:
                    child = command
                    if LIBS[name]:
                        child = ['env', f'LD_PRELOAD={LIBS[name]}', *command]
                    with output.open('wb') as stream:
                        result = subprocess.run(
                            ['python3', str(ROOT / 'bench/apps/run_timed.py'), *child],
                            env=env, stdout=stream, stderr=subprocess.PIPE,
                            text=True, check=True, timeout=600)
                    match = re.search(r'R3TIME elapsed=([\d.]+) maxrss_kb=(\d+) exit=0', result.stderr)
                    assert match, result.stderr
                    assert digest(output) == hashes[app], f'{app}/{name}: output mismatch'
                    sample = dict(app=app, allocator=name, rep=rep + 1,
                                  seconds=float(match[1]), rss_kib=int(match[2]))
                    samples.append(sample)
                    print(json.dumps(sample), flush=True)
        summary = []
        for app in commands:
            for name in LIBS:
                row = dict(app=app, allocator=name)
                for metric in ('seconds', 'rss_kib'):
                    values = [s[metric] for s in samples if s['app'] == app and s['allocator'] == name]
                    row[metric] = dict(median=statistics.median(values), min=min(values), max=max(values))
                summary.append(row)
        report = dict(reps=REPS, input_bytes=source.stat().st_size,
                      input_sha256=digest(source), output_sha256=hashes,
                      commands=commands, libraries=LIBS, samples=samples, summary=summary)
        target = Path(os.environ.get('RESULTS', str(work / 'data-tools.json')))
        target.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
