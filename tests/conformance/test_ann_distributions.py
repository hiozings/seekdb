#!/usr/bin/env python3
"""ANN recall on clustered, duplicated and uneven-scale finite float32 data."""
import argparse
import math
from pathlib import Path
import random
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol_client import Session

BINARY = Path(__file__).resolve().parents[2] / 'build/minivectordb'


def float32(value):
    return struct.unpack('f', struct.pack('f', value))[0]


def distributions():
    rng = random.Random(96174)
    count, dim, queries = 3000, 32, 40
    centers = [[rng.gauss(0, 15) for _ in range(dim)] for _ in range(20)]
    rows = [[float32(x + rng.gauss(0, .15)) for x in centers[i % 20]]
            for i in range(count)]
    query_rows = []
    for _ in range(queries):
        a, b = rng.sample(centers, 2)
        query_rows.append([float32((x+y)/2 + rng.gauss(0, .1)) for x, y in zip(a, b)])
    yield 'clustered_midpoints', rows, query_rows

    centers = [[float32(rng.gauss(0, 1)) for _ in range(dim)] for _ in range(100)]
    yield 'duplicate_heavy', [centers[i % 100] for i in range(count)], [rng.choice(centers) for _ in range(queries)]

    def scaled_vector():
        scale = 10 ** rng.uniform(-25, 25)
        return [float32(rng.gauss(0, 1) * scale) for _ in range(dim)]
    yield 'uneven_magnitude', [scaled_vector() for _ in range(count)], [scaled_vector() for _ in range(queries)]


class AnnDistributions(unittest.TestCase):
    def test_distribution_recall_and_ties(self):
        for name, vectors, queries in distributions():
            with self.subTest(distribution=name), tempfile.TemporaryDirectory(prefix='mvdb-ann-distribution-') as directory:
                session = Session(BINARY, Path(directory))
                try:
                    def execute(sql, mode='exact'):
                        response = session.execute(sql, mode)
                        self.assertIs(response.get('ok'), True, response)
                        return response.get('rows')

                    def literal(vector):
                        return '[' + ','.join(repr(x) for x in vector) + ']'

                    ids = list(range(1, len(vectors)+1))
                    random.Random(174).shuffle(ids)
                    execute('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(32))')
                    for start in range(0, len(vectors), 100):
                        execute('INSERT INTO t VALUES ' + ','.join(
                            f"({pk},'{literal(vector)}')" for pk, vector in
                            zip(ids[start:start+100], vectors[start:start+100])))
                    execute('CREATE VECTOR INDEX ix ON t(v)')
                    recalls = []
                    for query in queries:
                        # Operands have already been quantized to float32;
                        # Python subtraction and accumulation use double.
                        oracle = [pk for _, pk in sorted(
                            (math.sqrt(math.fsum((x-y)**2 for x, y in zip(vector, query))), pk)
                            for pk, vector in zip(ids, vectors))[:10]]
                        sql = f"SELECT id FROM t ORDER BY L2_DISTANCE(v,'{literal(query)}') LIMIT 10"
                        exact = [row[0] for row in execute(sql)]
                        self.assertEqual(exact, oracle)
                        actual = [row[0] for row in execute(sql, 'ann')]
                        self.assertEqual(len(actual), 10)
                        self.assertEqual(len(set(actual)), 10)
                        self.assertTrue(set(actual).issubset(ids))
                        if name == 'duplicate_heavy':
                            self.assertEqual(actual, oracle, 'equal distances must use global primary-key order')
                        recalls.append(len(set(actual) & set(oracle)) / 10)
                    recall = sum(recalls) / len(recalls)
                    print(f'{name}: average Recall@10={recall:.6f}', flush=True)
                    self.assertGreaterEqual(recall, .95)
                finally:
                    session.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', type=Path, default=BINARY)
    args, rest = parser.parse_known_args()
    BINARY = args.bin.resolve()
    unittest.main(argv=[sys.argv[0], *rest])
