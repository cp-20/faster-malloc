"""Validate complete sample sets; write compact, reviewable measurement tables."""
from collections import defaultdict
import json
from pathlib import Path
import random
import statistics as st

root = Path('bench/results/round12-startup')

def summarize(rows, keys, metrics, count):
    groups = defaultdict(list)
    for row in rows:
        assert row.get('rc', 0) == 0, row
        groups[tuple(row[k] for k in keys)].append(row)
    result = []
    for key, values in groups.items():
        assert len(values) == count and {r['rep'] for r in values} == set(range(count)), key
        stats = {}
        for metric in metrics:
            xs = [r[metric] for r in values]
            stats[metric] = dict(median=st.median(xs), min=min(xs), max=max(xs))
        result.append(dict(zip(keys, key)) | stats)
    return result

result = {}
for name, keys, metrics, count in [
    ('baseline-startup', ['app','variant','method'], ['rss_kb','seconds'], 7),
    ('first16-startup', ['app','variant','method'], ['rss_kb','seconds'], 7),
    ('final-startup', ['app','variant','method'], ['rss_kb','seconds'], 7),
    ('first16-guard', ['app','variant'], ['seconds','rss_kb'], 7),
    ('first16-guard-cpu', ['app','variant'], ['seconds','cpu_seconds','rss_kb'], 9),
    ('final-guard', ['app','variant'], ['seconds','cpu_seconds','rss_kb'], 5),
]:
    rows = [json.loads(s) for s in (root / (name+'.jsonl')).read_text().splitlines()]
    result[name] = summarize(rows, keys, metrics, count)
    if 'guard' in name:
        assert len(rows) == count * 3 * 4
    if name in {'first16-startup','final-startup'}:
        assert len(rows) == 7 * 2 * len({r['variant'] for r in rows}) * 2
        for app in ['python','node']:
            for build in (['v2','jit'] if name == 'first16-startup' else ['v2']):
                for method in ['round10','target-only']:
                    old = [r['rss_kb'] for r in rows if (r['app'],r['variant'],r['method']) == (app,'base-'+build,method)]
                    new = [r['rss_kb'] for r in rows if (r['app'],r['variant'],r['method']) == (app,build,method)]
                    assert max(new) < min(old), (app, build, method)
    if name == 'final-guard':
        for app in ['python','node','kvstore']:
            for build in ['v2','jit']:
                for metric in ['seconds','cpu_seconds']:
                    old = [r[metric] for r in rows if (r['app'],r['variant']) == (app,'base-'+build)]
                    new = [r[metric] for r in rows if (r['app'],r['variant']) == (app,build)]
                    assert max(min(old),min(new)) <= min(max(old),max(new)), (app,build,metric)
    if name.endswith('-cpu'):
        pairs = []
        rng = random.Random(12)
        for app in ['python','node','kvstore']:
            for build in ['v2','jit']:
                for metric in ['seconds','cpu_seconds']:
                    old = {r['rep']:r[metric] for r in rows if (r['app'],r['variant']) == (app,'base-'+build)}
                    new = {r['rep']:r[metric] for r in rows if (r['app'],r['variant']) == (app,build)}
                    deltas = [100*(new[i]/old[i]-1) for i in old]
                    bootstrap = sorted(st.median(rng.choices(deltas,k=count)) for _ in range(10000))
                    pairs.append(dict(app=app, build=build, metric=metric,
                                      paired_median_pct=st.median(deltas),
                                      bootstrap95_pct=[bootstrap[250],bootstrap[9750]],
                                      range_overlap=max(min(old.values()),min(new.values())) <= min(max(old.values()),max(new.values()))))
        result['paired-cpu-guard'] = pairs
# Pool within-pass ratios, preserving every original guard pair.
pooled = []
for app in ['python','node','kvstore']:
    for build in ['v2','jit']:
        deltas = []
        filenames = ['first16-guard','first16-guard-cpu'] + (['final-guard'] if build == 'v2' else [])
        for filename in filenames:
            rows = [json.loads(s) for s in (root/(filename+'.jsonl')).read_text().splitlines()]
            old = {r['rep']:r['seconds'] for r in rows if (r['app'],r['variant']) == (app,'base-'+build)}
            new = {r['rep']:r['seconds'] for r in rows if (r['app'],r['variant']) == (app,build)}
            deltas.extend(100*(new[i]/old[i]-1) for i in old)
        rng = random.Random(12)
        bootstrap = sorted(st.median(rng.choices(deltas,k=len(deltas))) for _ in range(10000))
        pooled.append(dict(app=app,build=build,pairs=len(deltas),paired_median_pct=st.median(deltas),
                           bootstrap95_pct=[bootstrap[250],bootstrap[9750]]))
result['pooled-wall-guard'] = pooled
# Static adoption requires no resolved slowdown; the JIT experiment is rejected.
assert all(r['bootstrap95_pct'][0] <= 0 for r in pooled if r['build']=='v2')
assert next(r for r in pooled if (r['app'],r['build']) == ('kvstore','jit'))['bootstrap95_pct'][0] > 0
for build in ['release','debug']:
    assert (root/(build+'-round7.log')).read_text().rstrip().endswith('epoch stalled-reader grace ok')
    assert 'all 129' in (root/(build+'-smallspan.log')).read_text()
assert (root/'debug-purge.log').read_text().count('stress ok') == 6
(root/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
print('startup separation, sample counts, checked outputs and correctness logs verified')
for row in result.get('paired-cpu-guard',[]):
    print(row)
