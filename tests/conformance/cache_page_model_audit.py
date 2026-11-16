#!/usr/bin/env python3
"""Independent deterministic model audit; assertions compare user-visible state."""
import argparse
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('--bin',type=Path,required=True)
parser.add_argument('--seed',type=int,default=764)
args=parser.parse_args()
B=args.bin.resolve()
import sys,tempfile,json,random,math,struct
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from protocol_client import Session
r=random.Random(args.seed)
f32=lambda x:struct.unpack('f',struct.pack('f',x))[0]
with tempfile.TemporaryDirectory() as d:
 s=Session(B,Path(d))
 try:
  def ex(q):
   try:
    x=s.execute(q)
   except Exception:
    if s.proc is not None and s.proc.poll() is not None:
     print(s.proc.stderr.read(),file=sys.stderr)
    print('failed SQL:',q,file=sys.stderr)
    raise
   assert x.get('ok'),(q,x)
   return x.get('rows')
  for dim in (1,7,8,9,16,17,33):
   for t,width in [('cache',8),('page',1000000000)]:ex(f'CREATE TABLE {t}{dim}(id BIGINT PRIMARY KEY,s VARCHAR({width}),n BIGINT,v VECTOR({dim}))')
   vals=[]
   for i in range(50):
    v=[f32(r.uniform(-1,1)*10**r.randrange(-30,31)) for j in range(dim)]
    row=(i,r.choice(['','a','aa','中','😀']),r.randrange(-4,5),v);vals.append(row)
   for t in ['cache','page']:ex(f'INSERT INTO {t}{dim} VALUES '+','.join(f"({i},'{s}',{n},'{json.dumps(v)}')" for i,s,n,v in vals))
   for qi in range(50):
    q=[f32(r.uniform(-1,1)*10**r.randrange(-30,31)) for _ in range(dim)]
    limit=r.choice([0,1,3,10,60]);n=r.randrange(-4,5);needle=r.choice(['','a','aa','中','😀','zzzzzzzzzz'])
    sql="SELECT id,L2_DISTANCE(v,'"+json.dumps(q)+"') AS d FROM {} WHERE n >= "+str(n)+" AND s < '"+needle+"' ORDER BY d LIMIT "+str(limit)
    got=ex(sql.format('cache'+str(dim))); page=ex(sql.format('page'+str(dim)));assert got==page,(dim,got,page)
    oracle=sorted((math.sqrt(math.fsum((x-y)**2 for x,y in zip(v,q))),i) for i,sv,nv,v in vals if nv>=n and sv<needle)[:limit]
    assert [i for i,d in got]==[i for d,i in oracle],(dim,got,oracle)
    for (i,d),(expected,pk) in zip(got,oracle):assert math.isclose(d,expected,rel_tol=1e-12),(dim,d,expected)
  print('350 cache/page/oracle queries passed',flush=True)
 finally:
  s.close()
