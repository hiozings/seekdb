#!/usr/bin/env python3
"""Docker cgroup regression: 120 MB data in 96 MiB RAM, swap disabled.

Run after docker build: python3 tests/storage/container_memory_check.py --image IMAGE
Docker access is required; this is separate from CTest's host process tests.
"""
import argparse
import json,subprocess,tempfile
from pathlib import Path
parser = argparse.ArgumentParser()
parser.add_argument('--image', default='minivectordb:latest')
parser.add_argument('--data-root', type=Path, default=Path(__file__).resolve().parents[2],
                    help='disk-backed directory; do not use tmpfs with swap disabled')
args = parser.parse_args()
IMAGE = args.image
with tempfile.TemporaryDirectory(prefix='mvdb-cgroup-', dir=args.data_root) as d:
 def start():
  p=subprocess.Popen(['docker','run','--rm','-i','--memory=96m','--memory-swap=96m','--cpus=2',
    '--mount',f'type=bind,source={d},target=/data',IMAGE],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
  assert json.loads(p.stdout.readline())=={'type':'ready'}
  return p
 p=start(); seq=0
 def request(sql=None):
  global seq
  seq+=1
  req={'id':seq,'op':'shutdown'} if sql is None else {'id':seq,'op':'execute','sql':sql,'search_mode':'ann'}
  p.stdin.write(json.dumps(req)+'\n');p.stdin.flush()
  line=p.stdout.readline()
  if not line: raise RuntimeError(f'container stopped at request {seq}, exit={p.wait()}: '+p.stderr.read())
  r=json.loads(line);assert r.get('ok') is True,r
  return r
 request('CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR(6000),v VECTOR(1))')
 for a in range(0,20000,100):
  request('INSERT INTO t VALUES '+','.join(f"({i},'"+'x'*6000+"','[1]')" for i in range(a,a+100)))
 print('loaded 120 MB payload in 96 MiB container',flush=True)
 assert request('SELECT * FROM t ORDER BY id DESC LIMIT 1')['rows']==[[19999,'x'*6000,[1]]]
 request('CREATE VECTOR INDEX ix ON t(v)')
 assert request("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0]') LIMIT 1")['rows']==[[0]]
 print('bounded sorting and vector fallback PASS',flush=True)
 request('BEGIN');request("UPDATE t SET s='short'");request('ROLLBACK')
 assert request('SELECT s FROM t WHERE id=123')['rows']==[['x'*6000]]
 print('large transaction rollback PASS',flush=True)
 request();p.stdin.close();assert p.wait(timeout=60)==0;p.stdout.close();p.stderr.close()
 p=start()
 assert request('SELECT s FROM t WHERE id=123')['rows']==[['x'*6000]]
 request();p.stdin.close();assert p.wait(timeout=60)==0;p.stdout.close();p.stderr.close()
 print('restart verification PASS',flush=True)
