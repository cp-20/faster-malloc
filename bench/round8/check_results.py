import json,collections,statistics as st
from pathlib import Path
from summarize import summary
assert summary([0,0,0])["cv_pct"] is None
assert summary([1,2,3])["median"] == 2 and summary([1,2,3])["variance"] == 1
base=Path('bench/results/round8')
r=[json.loads(x) for x in (base/'realloc.jsonl').read_text().splitlines()]
assert len(r)==160,len(r)
for app in ['lua','jq','ruby','python']:
 for build in ['v2','jit']:
  for mode in [0,1]:
   subset=[x for x in r if (x['app'],x['build'],x['mode'])==(app,build,mode)]
   assert len(subset)==10 and sum(not x['warmup'] for x in subset)==9
   for x in subset:
    assert x['seconds']>0 and x['rss_kib']>0
    if app=='ruby':assert json.loads(x['output'])['checksum']==32103
    else: assert x['output']=={'lua':'lua-churn checksum\t19466078','jq':'66395','python':'python-churn checksum 625726'}[app]
r=[json.loads(x) for x in (base/'ownership.jsonl').read_text().splitlines()]
assert len(r)==80
for case in ['local','cross','many','census']:
 for build in ['round6','round7']:
  sub=[x for x in r if (x['case'],x['build'])==(case,build)]
  assert len(sub)==10 and sum(not x['warmup'] for x in sub)==9
excluded={x['index'] for x in json.loads((base/'matrix-exclusions.json').read_text())}
r=[json.loads(x) for x in (base/'matrix.jsonl').read_text().splitlines()]
valid=[x for i,x in enumerate(r) if i not in excluded]
assert len(valid)==105,(len(r),excluded)
keys=collections.Counter((x['app'],x['allocator']) for x in valid)
assert len(keys)==21 and set(keys.values())=={5}
assert len({(x['rep'],x['app'],x['allocator']) for x in valid})==105
assert {x['rep'] for x in valid}==set(range(5))
for x in valid:
 assert x['exit']==0 and not any(w in x['stdout'] for w in ['FAIL','MISSING','MISMATCH'])
 fields=x['stdout'].strip().splitlines()[-1].split()
 if x['app']=='ruby':assert fields[-1]=='32103'
 else:assert fields[-1]=='0'
 assert all(float(v)>0 for v in fields[1:-1]),x
print('all sample counts, app outputs, transfer assertions and matrix gates verified')
