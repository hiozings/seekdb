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

    def crash_restart(self):
        proc = self.session.proc
        proc.kill()
        proc.wait(timeout=10)
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            stream.close()
        self.session.proc = None
        self.session.start()

    def test_rollback_relocation_then_commit_survives_kill(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(7000))')
        self.execute("INSERT INTO t VALUES (1,'" + 'a'*4000 + "'),(2,'" + 'b'*4000 + "')")
        self.execute('BEGIN')
        self.execute("UPDATE t SET s = '" + 'c'*6000 + "' WHERE id = 1")
        self.execute('ROLLBACK')
        self.execute("INSERT INTO t VALUES (3,'new')")
        expected = [[1,'a'*4000],[2,'b'*4000],[3,'new']]
        self.assertEqual(self.rows('SELECT * FROM t ORDER BY id'), expected)
        self.crash_restart()
        self.assertEqual(self.rows('SELECT * FROM t ORDER BY id'), expected)
        response = self.session.execute("INSERT INTO t VALUES (1,'duplicate')")
        self.assertIs(response.get('ok'), False, response)
        self.assertEqual(response['error']['code'], 'duplicate_key')

    def test_failed_autocommit_insert_survives_kill(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(7000))')
        self.execute("INSERT INTO t VALUES (1,'" + 'a'*4000 + "'),(2,'" + 'b'*4000 + "')")
        # A new page is allocated before a later tuple conflicts with a live PK.
        response = self.session.execute("INSERT INTO t VALUES (3,'" + 'c'*6000 + "'),(1,'duplicate')")
        self.assertIs(response.get('ok'), False, response)
        self.execute("INSERT INTO t VALUES (3,'new')")
        expected = [[1,'a'*4000],[2,'b'*4000],[3,'new']]
        self.assertEqual(self.rows('SELECT * FROM t ORDER BY id'), expected)
        self.crash_restart()
        self.assertEqual(self.rows('SELECT * FROM t ORDER BY id'), expected)

    def test_bracket_strings_remain_varchar(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(30), v VECTOR(2))')
        self.execute("INSERT INTO t VALUES (1,'[note]','[1,2]'),(2,'[1,2]','[0,0]')")
        self.assertEqual(self.rows('SELECT id,s FROM t ORDER BY id'), [[1,'[note]'],[2,'[1,2]']])
        self.assertEqual(self.rows("SELECT id FROM t WHERE s = '[1,2]'"), [[2]])
        self.execute("UPDATE t SET s = '[changed]' WHERE id = 1")
        self.session.restart()
        self.assertEqual(self.rows('SELECT id,s FROM t ORDER BY id'), [[1,'[changed]'],[2,'[1,2]']])

    def test_star_projection_with_distance_order(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2))')
        self.execute("INSERT INTO t VALUES (3,'[1,0]'),(2,'[0,0]'),(1,'[1,0]')")
        self.execute('CREATE VECTOR INDEX ix ON t(v)')
        for mode in ('exact', 'ann'):
            for direction, expected in [('ASC', [[2,[0,0]],[1,[1,0]]]),
                                        ('DESC', [[1,[1,0]],[3,[1,0]]])]:
                with self.subTest(mode=mode, direction=direction):
                    response = self.session.execute(
                        f"SELECT * FROM t ORDER BY L2_DISTANCE(v,'[0,0]') {direction} LIMIT 2", mode)
                    self.assertIs(response.get('ok'), True, response)
                    self.assertEqual(response['columns'], ['id','v'])
                    self.assertEqual(response['rows'], expected)

    def test_malformed_vector_separators_are_rejected(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2))')
        for vector in ('[1 2]', '[1,,2]', '[,1,2]', '[1,2,]'):
            with self.subTest(vector=vector):
                response = self.session.execute(f"INSERT INTO t VALUES (1,'{vector}')")
                self.assertIs(response.get('ok'), False, response)
                self.assertEqual(self.rows('SELECT id FROM t'), [])
        self.execute("INSERT INTO t VALUES (1,'[ 1, 2 ]')")
        self.assertEqual(self.rows('SELECT * FROM t'), [[1,[1,2]]])

    def test_transactional_create_commit_survives_kill(self):
        self.execute('BEGIN')
        self.execute('CREATE TABLE born (id BIGINT PRIMARY KEY, v VECTOR(2))')
        self.execute("INSERT INTO born VALUES (1,'[1,2]')")
        self.execute('CREATE VECTOR INDEX ix ON born(v)')
        self.execute('COMMIT')
        self.crash_restart()
        self.assertEqual(self.rows('SELECT * FROM born'), [[1,[1,2]]])
        response = self.session.execute("SELECT id FROM born ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 1", 'ann')
        self.assertIs(response.get('ok'), True, response)
        self.assertEqual(response['rows'], [[1]])

    def test_uncommitted_catalog_is_not_recovered(self):
        self.execute('CREATE TABLE kept (id BIGINT PRIMARY KEY, v VECTOR(2))')
        self.execute("INSERT INTO kept VALUES (1,'[1,2]')")
        self.execute('BEGIN')
        self.execute('CREATE TABLE ghost (id BIGINT PRIMARY KEY)')
        self.execute('INSERT INTO ghost VALUES (99)')
        self.execute('CREATE VECTOR INDEX ix ON kept(v)')
        self.crash_restart()
        response = self.session.execute('SELECT * FROM ghost')
        self.assertIs(response.get('ok'), False, response)
        self.assertEqual(response['error']['code'], 'unknown_table')
        self.execute('CREATE VECTOR INDEX ix ON kept(v)')
        self.execute('CREATE TABLE ghost (id BIGINT PRIMARY KEY)')
        self.execute('INSERT INTO ghost VALUES (42)')
        self.crash_restart()
        self.assertEqual(self.rows('SELECT * FROM ghost'), [[42]])
        self.assertEqual(self.rows('SELECT * FROM kept'), [[1,[1,2]]])

    def test_huge_limit_does_not_preallocate_huge_memory(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2))')
        self.execute("INSERT INTO t VALUES (1,'[0,0]')")
        self.assertEqual(self.rows("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 9223372036854775807"), [[1]])

    def test_high_cardinality_filter_has_bounded_memory(self):
        import resource
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(2))')
        for start in range(0, 30000, 500):
            self.execute('INSERT INTO t VALUES ' + ','.join(
                f"({i},'[0,0]')" for i in range(start, start+500)))
        resource.prlimit(self.session.proc.pid, resource.RLIMIT_AS, (96*1024*1024, 96*1024*1024))
        self.assertEqual(self.rows("SELECT id FROM t WHERE id = 123 ORDER BY L2_DISTANCE(v,'[0,0]') LIMIT 1"), [[123]])

    def test_large_mutation_with_memory_budget(self):
        import resource
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(4000))')
        payload = 'x'*3500
        for start in range(0, 20000, 100):
            self.execute('INSERT INTO t VALUES ' + ','.join(
                f"({i},'{payload}')" for i in range(start, start+100)))
        resource.prlimit(self.session.proc.pid, resource.RLIMIT_AS, (96*1024*1024, 96*1024*1024))
        self.execute('BEGIN')
        self.assertEqual(self.execute("UPDATE t SET s = 'short'")['affected_rows'], 20000)
        self.execute('ROLLBACK')
        self.assertEqual(self.rows('SELECT id,s FROM t WHERE id = 123'), [[123,payload]])
        self.assertEqual(self.execute('DELETE FROM t')['affected_rows'], 20000)
        self.crash_restart()
        self.assertEqual(self.rows('SELECT * FROM t'), [])

    def test_varchar_filter_is_not_storage_assignment(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(1))')
        self.execute("INSERT INTO t VALUES (1,'a')")
        self.assertEqual(self.rows("SELECT id FROM t WHERE s < 'xx'"), [[1]])
        self.assertEqual(self.execute("UPDATE t SET s = 'b' WHERE s < 'xx'")['affected_rows'], 1)
        self.assertEqual(self.execute("DELETE FROM t WHERE s = 'xx'")['affected_rows'], 0)

    def test_ann_ties_use_primary_key_order(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(1))')
        self.execute('INSERT INTO t VALUES ' + ','.join(f"({i},'[1]')" for i in range(100,0,-1)))
        self.execute('CREATE VECTOR INDEX ix ON t(v)')
        response = self.session.execute("SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0]') LIMIT 10", 'ann')
        self.assertIs(response.get('ok'), True, response)
        self.assertEqual(response['rows'], [[i] for i in range(1,11)])

    def test_large_projection_and_index_respect_memory_budget(self):
        import resource
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(6000), v VECTOR(1))')
        payload = 'x'*6000
        for start in range(0, 20000, 100):
            self.execute('INSERT INTO t VALUES '+','.join(
                f"({i},'{payload}','[1]')" for i in range(start,start+100)))
        resource.prlimit(self.session.proc.pid, resource.RLIMIT_AS, (96*1024*1024,96*1024*1024))
        self.assertEqual(self.rows('SELECT * FROM t ORDER BY id DESC LIMIT 1'), [[19999,payload,[1]]])
        query = "SELECT id FROM t ORDER BY L2_DISTANCE(v,'[0]') LIMIT 1"
        self.assertEqual(self.rows(query), [[0]])
        self.execute('CREATE VECTOR INDEX ix ON t(v)')
        response = self.session.execute(query, 'ann')
        self.assertIs(response.get('ok'), True, response)
        self.assertEqual(response['rows'], [[0]])

    def test_undo_io_failure_never_commits_partial_mutation(self):
        # Deterministically deny creation of the next undo file without touching
        # already open heap/WAL files, as an ENOSPC-like write failure injection.
        from protocol_client import ProtocolError
        data = Path(self.directory.name)
        moved = data.with_name(data.name + '-moved')
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY)')
        self.execute('INSERT INTO t VALUES (1)')
        data.rename(moved)
        proc = self.session.proc
        try:
            with self.assertRaises(ProtocolError):
                self.session.execute('INSERT INTO t VALUES (2)')
            self.assertNotEqual(proc.wait(timeout=10), 0)
        finally:
            moved.rename(data)
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=10)
            for stream in (proc.stdin, proc.stdout, proc.stderr):
                stream.close()
            self.session.proc = None
        self.session.start()
        self.assertEqual(self.rows('SELECT * FROM t ORDER BY id'), [[1]])

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
        response = self.session.execute("UPDATE t SET s = '" + 'x'*11000 + "' WHERE id = 1")
        self.assertIs(response.get('ok'), False, response)
        self.assertEqual(self.rows('SELECT * FROM t'), [[1,'old']])
        self.session.restart()
        self.assertEqual(self.rows('SELECT * FROM t'), [[1,'old']])

    def test_failed_update_preserves_transaction(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(10000))')
        self.execute("INSERT INTO t VALUES (1,'old')")
        self.execute('BEGIN')
        self.execute("INSERT INTO t VALUES (2,'prior')")
        response = self.session.execute("UPDATE t SET s = '" + 'x'*11000 + "' WHERE id = 1")
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
