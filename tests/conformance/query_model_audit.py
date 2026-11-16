#!/usr/bin/env python3
"""Independent deterministic model audit; assertions compare user-visible state."""
import argparse
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('--bin',type=Path,required=True)
parser.add_argument('--seed',type=int,default=4234)
args=parser.parse_args()
B=args.bin.resolve()
import subprocess,json,tempfile,random,math,functools
directory=tempfile.TemporaryDirectory(prefix='mvdb-query-model-')
p=subprocess.Popen([str(B),'--data-dir',directory.name],stdin=subprocess.PIPE,stdout=subprocess.PIPE,text=True)
assert json.loads(p.stdout.readline())=={'type':'ready'}
n=0
def q(s):
 global n
 n+=1;p.stdin.write(json.dumps({'id':n,'op':'execute','sql':s})+'\n');p.stdin.flush();r=json.loads(p.stdout.readline());assert r['id']==n;return r
def ok(s):
 r=q(s);assert r['ok'],(s,r);return r
ok('CREATE TABLE t (s VARCHAR(8), x BIGINT, id BIGINT PRIMARY KEY, v VECTOR(2))')
random.seed(args.seed)
rows=[]
for i in range(100): rows.append({'id':i-50,'x':random.choice([-9223372036854775808,9223372036854775807,-2,0,1,9007199254740993]),'s':random.choice(['a','é','😀','中',"a'b",'','z','\x00']),'v':[random.randint(-3,3),random.randint(-3,3)]})
def lit(x):return "'"+x.replace("'","''")+"'" if isinstance(x,str) else str(x)
for r in rows:ok('INSERT INTO t VALUES ('+','.join([lit(r['s']),str(r['x']),str(r['id']),lit(json.dumps(r['v']))])+')')
ops={'=':lambda a,b:a==b,'<':lambda a,b:a<b,'<=':lambda a,b:a<=b,'>':lambda a,b:a>b,'>=':lambda a,b:a>=b}
for i in range(1200):
 selected=rows[:];conds=[]
 for _ in range(random.randint(0,3)):
  col=random.choice(['id','x','s']); val=random.choice(rows)[col];op=random.choice(list(ops));conds.append(col+op+lit(val));selected=[r for r in selected if ops[op](r[col],val)]
 where=' WHERE '+' AND '.join(conds) if conds else ''
 keys=random.sample(['id','x','s'],random.randint(1,3));dirs=[random.choice([1,-1]) for _ in keys]
 def cmp(a,b):
  for k,d in zip(keys,dirs):
   if a[k]!=b[k]:return d*((a[k]>b[k])-(a[k]<b[k]))
  return (a['id']>b['id'])-(a['id']<b['id'])
 selected.sort(key=functools.cmp_to_key(cmp));limit=random.choice([0,1,2,10,100,9223372036854775807]);selected=selected[:limit]
 sql='SELECT id,s,x FROM t'+where+' ORDER BY '+','.join(k+(' ASC' if d==1 else ' DESC') for k,d in zip(keys,dirs))+' LIMIT '+str(limit)
 got=ok(sql)['rows'];assert got==[[r['id'],r['s'],r['x']] for r in selected],(sql,got,selected)
for i in range(500):
 query=[random.randint(-3,3),random.randint(-3,3)]; col=random.choice(['s','x','id']);val=random.choice(rows)[col];op=random.choice(list(ops));lim=random.choice([0,1,3,30,100]);sel=[r for r in rows if ops[op](r[col],val)];sel.sort(key=lambda r:(math.dist(r['v'],query),r['id']));sel=sel[:lim]
 sql='SELECT id,L2_DISTANCE(v,'+lit(json.dumps(query))+') AS d FROM t WHERE '+col+op+lit(val)+' ORDER BY d LIMIT '+str(lim)
 got=ok(sql)['rows'];assert [r[0] for r in got]==[r['id'] for r in sel],(sql,got,sel)
for bad in ["(100,'bad')","(100,1,900,'[nan,0]')","('ok',1,900,'[0,0]'),('toolong!!!',1,901,'[0,0]')","('ok',1,900,'[0,0]'),('ok',1,901,'[3.5e38,0]')","('ok',1,900,'[0,0]'),('ok',1,901,'[0]')"]:
 ok('BEGIN'); assert not q('INSERT INTO t VALUES '+bad)['ok']; assert ok('SELECT id FROM t WHERE id>=900')['rows']==[];ok('COMMIT')
# Exercise int64 primary keys through an index, with a non-leading PK column.
ok('CREATE TABLE pk_edges(v VECTOR(1),id BIGINT PRIMARY KEY,s VARCHAR(3))')
edge_ids=[-9223372036854775808,9223372036854775807,-1,0,1,9007199254740992,9007199254740993,-9007199254740992,-9007199254740993]
ok('INSERT INTO pk_edges VALUES '+','.join(f"('[0]',{key},'中')" for key in reversed(edge_ids)))
assert ok('SELECT id FROM pk_edges ORDER BY id')['rows']==[[key] for key in sorted(edge_ids)]
for key in edge_ids:
 assert ok(f'SELECT id FROM pk_edges WHERE id={key}')['rows']==[[key]]
 assert not q(f"INSERT INTO pk_edges VALUES('[0]',{key},'dup')")['ok']
assert ok("SELECT id FROM pk_edges ORDER BY L2_DISTANCE(v,'[0]') LIMIT 3")['rows']==[[key] for key in sorted(edge_ids)[:3]]
print('PASSED',n,'requests: 1200 scalar differential, 500 exact differential, atomicity and Unicode/INT64 boundaries')
p.stdin.close();assert p.wait(timeout=30)==0
p.stdout.close()
directory.cleanup()
