#!/usr/bin/env python3
"""Filtered ANN recall regression against an independent float32 brute-force oracle."""
import argparse
import json
import struct
from pathlib import Path
import random
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from protocol_client import Session

class FilteredAnn(unittest.TestCase):
    def test_sparse_filter_recall(self):
        rng = random.Random(419)
        count, dim = 24000, 128
        def vector():
            return '['+','.join(format(rng.uniform(-1,1), '.9g') for _ in range(dim))+']'
        with tempfile.TemporaryDirectory(prefix='mvdb-filter-recall-') as directory:
            session = Session(BINARY,Path(directory))
            try:
                def execute(sql,mode='exact'):
                    response=session.execute(sql,mode)
                    self.assertIs(response.get('ok'),True,response)
                    return response.get('rows')
                execute(f'CREATE TABLE t(id BIGINT PRIMARY KEY,g BIGINT,v VECTOR({dim}))')
                def stored_vector(literal):
                    return [struct.unpack('f',struct.pack('f',x))[0] for x in json.loads(literal)]
                filtered=[]
                for start in range(0,count,100):
                    tuples=[]
                    for i in range(start,start+100):
                        literal=vector()
                        tuples.append(f"({i},{i%100},'{literal}')")
                        if i%100<2:
                            filtered.append((i,stored_vector(literal)))
                    execute('INSERT INTO t VALUES '+','.join(tuples))
                execute('CREATE VECTOR INDEX ix ON t(v)')
                recalls=[]
                for _ in range(100):
                    literal=vector()
                    query=stored_vector(literal)
                    oracle=[pk for _,pk in sorted(
                        (sum((x-y)**2 for x,y in zip(row,query)),pk) for pk,row in filtered)[:10]]
                    sql=f"SELECT id FROM t WHERE g < 2 ORDER BY L2_DISTANCE(v,'{literal}') LIMIT 10"
                    exact=[r[0] for r in execute(sql)]
                    self.assertEqual(exact,oracle)
                    ann=[r[0] for r in execute(sql,'ann')]
                    self.assertEqual(len(ann),10)
                    self.assertEqual(len(set(ann)),10)
                    self.assertTrue(all(i%100<2 for i in ann))
                    recalls.append(len(set(exact)&set(ann))/10)
                recall=sum(recalls)/len(recalls)
                self.assertGreaterEqual(recall,0.95,f'average Recall@10 = {recall:.6f}')
            finally:
                session.close()

if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--bin',type=Path,required=True)
    args,rest=parser.parse_known_args()
    BINARY=args.bin.resolve()
    unittest.main(argv=[sys.argv[0],*rest])
