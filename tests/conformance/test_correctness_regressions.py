#!/usr/bin/env python3
"""Regression tests for the four ef93a008 correctness defects.

Run: python3 tests/conformance/test_correctness_regressions.py --bin build/minivectordb
"""
import argparse
import math
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol_client import Session

BINARY = Path(__file__).resolve().parents[2] / 'build/minivectordb'


class CorrectnessRegressions(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix='mvdb-regression-')
        self.addCleanup(self.directory.cleanup)
        self.session = Session(BINARY, Path(self.directory.name))
        self.addCleanup(self.session.close)

    def execute(self, sql):
        response = self.session.execute(sql)
        self.assertIs(response.get('ok'), True, response)
        return response

    def rows(self, sql):
        return self.execute(sql)['rows']

    def test_bigint_minimum_and_overflow(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, n BIGINT)')
        self.execute('INSERT INTO t VALUES (1,-9223372036854775808)')
        self.assertEqual(self.rows('SELECT n FROM t'), [[-9223372036854775808]])
        for value in ('9223372036854775808', '-9223372036854775809'):
            response = self.session.execute(f'INSERT INTO t VALUES (2,{value})')
            self.assertIs(response.get('ok'), False, response)
        self.assertEqual(self.rows('SELECT id FROM t'), [[1]])

    def test_bigint_comparison_and_mutations(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, n BIGINT)')
        self.execute('INSERT INTO t VALUES (1,9007199254740993),(2,9007199254740992),'
                     '(3,-9223372036854775807),(4,-9223372036854775806),'
                     '(5,9223372036854775807),(6,9223372036854775806)')
        for op, expected in [('=', [[2]]), ('<', [[3], [4]]), ('<=', [[2], [3], [4]]),
                             ('>', [[1], [5], [6]]), ('>=', [[1], [2], [5], [6]])]:
            with self.subTest(op=op):
                self.assertEqual(self.rows(f'SELECT id FROM t WHERE n {op} 9007199254740992 ORDER BY id'), expected)
        self.assertEqual(self.rows('SELECT id FROM t ORDER BY n'), [[3], [4], [2], [1], [6], [5]])
        self.assertEqual(self.execute('UPDATE t SET n = 0 WHERE n = -9223372036854775807')['affected_rows'], 1)
        self.assertEqual(self.execute('DELETE FROM t WHERE n = 9223372036854775807')['affected_rows'], 1)
        self.assertEqual(self.rows('SELECT id FROM t ORDER BY id'), [[1], [2], [3], [4], [6]])

    def test_rollback_update_then_delete(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, n BIGINT, v VECTOR(1))')
        self.execute("INSERT INTO t VALUES (1,10,'[1]'),(2,20,'[2]')")
        self.execute('CREATE VECTOR INDEX ix ON t(v)')
        self.execute('BEGIN')
        self.execute("UPDATE t SET n = 11, v = '[3]' WHERE id = 1")
        self.execute('DELETE FROM t')
        self.execute('ROLLBACK')
        self.assertEqual(self.rows('SELECT id,n FROM t ORDER BY id'), [[1,10],[2,20]])
        response = self.session.execute("SELECT id,L2_DISTANCE(v,'[0]') AS d FROM t ORDER BY d LIMIT 2", 'ann')
        self.assertIs(response.get('ok'), True, response)
        self.assertEqual(response['rows'], [[1,1],[2,2]])
        self.session.restart()
        self.assertEqual(self.rows('SELECT id,n FROM t ORDER BY id'), [[1,10],[2,20]])

    def test_rollback_insert_then_delete_reused_slots(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, n BIGINT)')
        self.execute('INSERT INTO t VALUES (2,20)')
        self.execute('BEGIN')
        self.execute('INSERT INTO t VALUES (1,10)')
        self.execute('DELETE FROM t')
        self.execute('ROLLBACK')
        self.assertEqual(self.rows('SELECT * FROM t ORDER BY id'), [[2,20]])
        self.execute('INSERT INTO t VALUES (1,30)')
        self.session.restart()
        self.assertEqual(self.rows('SELECT * FROM t ORDER BY id'), [[1,30],[2,20]])

    def test_failed_growing_update_autocommit(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(10000))')
        self.execute("INSERT INTO t VALUES (1,'old')")
        response = self.session.execute("UPDATE t SET s = '" + 'x'*9000 + "' WHERE id = 1")
        self.assertIs(response.get('ok'), False, response)
        self.assertEqual(self.rows('SELECT * FROM t'), [[1,'old']])
        self.session.restart()
        self.assertEqual(self.rows('SELECT * FROM t'), [[1,'old']])

    def test_failed_update_preserves_transaction(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(10000))')
        self.execute("INSERT INTO t VALUES (1,'old')")
        self.execute('BEGIN')
        self.execute("INSERT INTO t VALUES (2,'prior')")
        response = self.session.execute("UPDATE t SET s = '" + 'x'*9000 + "' WHERE id = 1")
        self.assertIs(response.get('ok'), False, response)
        self.assertEqual(self.rows('SELECT * FROM t ORDER BY id'), [[1,'old'],[2,'prior']])
        self.execute("UPDATE t SET s = 'successfully grown' WHERE id = 1")
        self.execute('COMMIT')
        self.session.restart()
        self.assertEqual(self.rows('SELECT * FROM t ORDER BY id'), [[1,'successfully grown'],[2,'prior']])

    def test_large_finite_vector_distance(self):
        # Exercise scalar, AVX2 block and tail paths. Independent float32 oracle.
        value = struct.unpack('f', struct.pack('f', 3e38))[0]
        for dim in (1, 8, 9):
            with self.subTest(dim=dim):
                self.execute(f'CREATE TABLE t{dim} (id BIGINT PRIMARY KEY, v VECTOR({dim}))')
                positive = '[' + ','.join(['3e38']*dim) + ']'
                negative = '[' + ','.join(['-3e38']*dim) + ']'
                self.execute(f"INSERT INTO t{dim} VALUES (1,'{positive}'),(2,'{negative}')")
                query = f"SELECT id,L2_DISTANCE(v,'{negative}') AS d FROM t{dim} ORDER BY d"
                for suffix in ('', ' LIMIT 2'):
                    rows = self.rows(query + suffix)
                    self.assertEqual([row[0] for row in rows], [2,1])
                    self.assertEqual(rows[0][1], 0)
                    self.assertTrue(math.isfinite(rows[1][1]))
                    self.assertTrue(math.isclose(rows[1][1], 2*value*math.sqrt(dim), rel_tol=1e-12))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', type=Path, default=BINARY)
    args, rest = parser.parse_known_args()
    BINARY = args.bin.resolve()
    unittest.main(argv=[sys.argv[0], *rest])
