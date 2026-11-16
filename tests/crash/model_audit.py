#!/usr/bin/env python3
"""Independent deterministic model audit; assertions compare user-visible state."""
import argparse
from pathlib import Path
parser=argparse.ArgumentParser()
parser.add_argument('--bin',type=Path,required=True)
parser.add_argument('--seeds',type=int,default=2)
args=parser.parse_args()
B=args.bin.resolve()
import os,sys,tempfile,random,json
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from protocol_client import Session
os.environ['MVDB_POOL_PAGES']='4'
def kill(s):
 p=s.proc;p.kill();p.wait();[x.close() for x in (p.stdin,p.stdout,p.stderr)];s.proc=None
def run(seed):
 r=random.Random(seed)
 with tempfile.TemporaryDirectory(prefix='audit-model-') as d:
  s=Session(B,Path(d))
  try:
   models={};log=[]
   def sql(q,ok=True):
    log.append(q[:200]);a=s.execute(q)
    if a.get('ok')!=ok: raise AssertionError((q[:200],a,log[-10:]))
    return a
   def verify():
    for t,m in models.items():
     a=sql(f'SELECT * FROM {t} ORDER BY id')['rows'];e=[[k,v] for k,v in sorted(m.items())]
     if a!=e:raise AssertionError(('MISMATCH',seed,t,len(a),len(e),[(x[0],len(x[1])) for x in a[:10]],log[-10:]))
   for t in ['t','u']:
    sql(f'CREATE TABLE {t}(id BIGINT PRIMARY KEY,s VARCHAR(30000))');models[t]={}
    for base in range(0,800,100):
     sql(f'INSERT INTO {t} VALUES '+','.join(f"({k},'start{k}')" for k in range(base,base+100)));models[t].update({k:f'start{k}' for k in range(base,base+100)})
   for epoch in range(18):
    before={t:m.copy() for t,m in models.items()};sql('BEGIN')
    for op in range(25):
     t=r.choice(list(models));m=models[t];k=r.randrange(1200);typ=r.randrange(5);v=chr(97+r.randrange(26))*r.choice([1,50,4000,9000,18000])
     if typ<=1:
      sql(f"INSERT INTO {t} VALUES ({k},'{v}')",k not in m)
      if k not in m:m[k]=v
     elif typ==2:
      sql(f"UPDATE {t} SET s='{v}' WHERE id={k}")
      if k in m:m[k]=v
     elif typ==3:
      sql(f'DELETE FROM {t} WHERE id={k}');m.pop(k,None)
     else:
      if m:
       dup=r.choice(list(m));new=2000+epoch*25+op
       sql(f"INSERT INTO {t} VALUES ({new},'{v}'),({dup},'bad')",False)
    outcome=r.randrange(4)
    if outcome==0:sql('ROLLBACK');models=before
    elif outcome==1:models=before
    else:sql('COMMIT')
    kill(s);s.start();verify()
    if epoch%3==0:
     sql('DROP TABLE u');sql('CREATE TABLE u(id BIGINT PRIMARY KEY,s VARCHAR(30000))');models['u']={}
     sql("INSERT INTO u VALUES (1,'recreated')");models['u'][1]='recreated'
     kill(s);s.start();verify()
  finally:
   s.close()
 print('PASS',seed,flush=True)
for seed in range(args.seeds):run(seed)
