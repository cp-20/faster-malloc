import json, statistics, collections, sys
p = sys.argv[1] if len(sys.argv) > 1 else 'bench/results/round10-shortlived/matrix.jsonl'
g = collections.defaultdict(list)
for l in open(p):
    r = json.loads(l)
    g[r['app'], r['variant']].append(r)
for k, rs in g.items():
    print(k, len(rs), *[f'{f}: {statistics.median(v):.3f} [{min(v):.3f}, {max(v):.3f}]' for f in ['rss_kb', 'seconds'] if (v := [r[f] for r in rs])])
