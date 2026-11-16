#!/usr/bin/env python3
"""Finite float32 ANN ranking regressions, including scalar and SIMD dimensions."""
import argparse
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol_client import Session

BINARY = Path(__file__).resolve().parents[2] / 'build/minivectordb'


class VectorEdges(unittest.TestCase):
    def test_page_scan_handles_unaligned_vectors(self):
        # A wide declared VARCHAR disables the optional cache. Variable byte
        # lengths put vectors at unaligned offsets in real encoded heap rows.
        with tempfile.TemporaryDirectory() as directory:
            session = Session(BINARY, Path(directory))
            try:
                for dim in (1, 8, 9):
                    vector = '[' + ','.join(['1']*dim) + ']'
                    statements = [
                        f'CREATE TABLE t{dim}(id BIGINT PRIMARY KEY,s VARCHAR(1000000000),v VECTOR({dim}))',
                        f"INSERT INTO t{dim} VALUES(1,'a','{vector}'),(2,'aa','{vector}'),(3,'aaa','{vector}')"]
                    for sql in statements:
                        self.assertIs(session.execute(sql).get('ok'), True)
                    response = session.execute(f"SELECT id FROM t{dim} ORDER BY L2_DISTANCE(v,'{vector}') LIMIT 2")
                    self.assertIs(response.get('ok'), True, response)
                    self.assertEqual(response['rows'], [[1],[2]])
            finally:
                session.close()

    def test_ann_extreme_finite_coordinates_preserve_neighbors(self):
        for dim in (1, 8):
            for scale in ('e20', 'e-30'):
                with self.subTest(dim=dim, scale=scale), tempfile.TemporaryDirectory() as directory:
                    session = Session(BINARY, Path(directory))
                    try:
                        def execute(sql, mode='exact'):
                            result = session.execute(sql, mode)
                            self.assertIs(result.get('ok'), True, result)
                            return result

                        def vector(i):
                            return '[' + ','.join([str(i) + scale] * dim) + ']'

                        execute(f'CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR({dim}))')
                        execute('INSERT INTO t VALUES ' + ','.join(
                            f"({i},'{vector(i)}')" for i in range(1, 101)))
                        execute('CREATE VECTOR INDEX vi ON t(v)')
                        sql = (f"SELECT id,L2_DISTANCE(v,'{vector(101)}') AS d "
                               'FROM t ORDER BY d LIMIT 10')
                        exact = execute(sql)['rows']
                        self.assertEqual([row[0] for row in exact], list(range(100, 90, -1)))
                        self.assertEqual(execute(sql, 'ann')['rows'], exact)
                    finally:
                        session.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', type=Path, default=BINARY)
    args, rest = parser.parse_known_args()
    BINARY = args.bin.resolve()
    unittest.main(argv=[sys.argv[0]] + rest)
