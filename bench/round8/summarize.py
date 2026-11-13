"""Summarize saved round-8 samples; sample SD/CV, never minimum-as-score."""
import collections
import json
import re
import statistics as st
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'results' / 'round8'

def summary(values):
    return dict(n=len(values), median=st.median(values), minimum=min(values),
                maximum=max(values), variance=st.variance(values) if len(values)>1 else 0,
                sd=st.stdev(values) if len(values)>1 else 0,
                cv_pct=(100*st.stdev(values)/st.mean(values) if len(values)>1 else 0)
                if st.mean(values) else None)


def summarize():
    groups = collections.defaultdict(lambda: collections.defaultdict(list))
    for filename in ('realloc.jsonl', 'ownership.jsonl'):
        for row in map(json.loads, (ROOT/filename).read_text().splitlines()):
            if row['warmup']:
                continue
            key = (f"realloc/{row['app']}/{row['build']}/{row['mode']}"
                   if filename.startswith('realloc') else f"ownership/{row['case']}/{row['build']}")
            for metric in ('seconds', 'rss_kib'):
                groups[key][metric].append(row[metric])
    matrix = ROOT/'matrix.jsonl'
    if matrix.exists():
        excluded_path=ROOT/'matrix-exclusions.json'
        excluded={x['index'] for x in json.loads(excluded_path.read_text())} if excluded_path.exists() else set()
        for index,row in enumerate(map(json.loads, matrix.read_text().splitlines())):
            if index in excluded:
                continue
            assert row['exit']==0 and not any(x in row['stdout'] for x in ('FAIL','MISSING','MISMATCH')), row
            lines=row['stdout'].strip().splitlines()
            data=lines[-1].split()
            assert data[0]==row['allocator'], row
            metrics={'nginx':['rps','p50_us','p99_us','rss_kib','errors'],
                     'pgbench':['tps','avg_ms','rss_kib','failed'],
                     'ruby':['seconds','rss_kib','checksum']}[row['app']]
            assert len(data)==len(metrics)+1,row
            for metric,value in zip(metrics,data[1:]):
                groups[f"matrix/{row['app']}/{row['allocator']}"][metric].append(float(value))
    profile=ROOT/'lockprice.txt'
    extra=ROOT/'lockprice-extra.txt'
    if profile.exists() and extra.exists():
        symbols={int(offset,16):name for offset,name in re.findall(
            r'([0-9a-f]+) b fm_(central|census)_lock',profile.read_text())}
        case=None
        for line in (profile.read_text()+extra.read_text()).splitlines():
            if line in ('local','cross','many','census'):
                case=line
            elif line.startswith('rep='):
                case=line.split('case=')[1]
            match=re.match(r'mutex_offset=(0x[0-9a-f]+) calls=(\d+) inclusive_lock_ns=(\d+)',line)
            if match:
                offset,calls,nanos=match.groups()
                key=f"lockprice/{case}/{symbols[int(offset,16)]}"
                groups[key]['calls'].append(int(calls))
                groups[key]['inclusive_seconds'].append(int(nanos)/1e9)
    return {key:{metric:summary(values) for metric,values in group.items()}
            for key,group in sorted(groups.items())}

if __name__ == '__main__':
    print(json.dumps(summarize(),indent=2))
