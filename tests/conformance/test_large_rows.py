#!/usr/bin/env python3
"""Large encoded rows span heap pages and retain normal transaction semantics."""
import argparse
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol_client import Session

BINARY = Path(__file__).resolve().parents[2] / 'build/minivectordb'


class LargeRows(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='mvdb-large-')
        self.addCleanup(directory.cleanup)
        self.session = Session(BINARY, Path(directory.name))
        self.addCleanup(self.session.close)

    def execute(self, sql, mode='exact'):
        result = self.session.execute(sql, mode)
        self.assertIs(result.get('ok'), True, result)
        return result

    def crash_restart(self):
        proc = self.session.proc
        proc.kill()
        proc.wait(timeout=10)
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            stream.close()
        self.session.proc = None
        self.session.start()

    def test_large_varchar_updates_and_restart(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(20000))')
        original = '雪' * 10000
        self.execute(f"INSERT INTO t VALUES (1,'{original}'),(2,'small')")
        self.assertEqual(self.execute('SELECT * FROM t ORDER BY id')['rows'],
                         [[1, original], [2, 'small']])
        self.execute("UPDATE t SET s='" + 'x' * 20000 + "' WHERE id=2")
        self.execute("UPDATE t SET s='short' WHERE id=1")
        self.session.restart()
        self.assertEqual(self.execute('SELECT * FROM t ORDER BY id')['rows'],
                         [[1, 'short'], [2, 'x' * 20000]])
        self.execute('DELETE FROM t WHERE id=2')
        self.execute("INSERT INTO t VALUES (3,'" + 'y' * 16000 + "')")
        self.crash_restart()
        self.assertEqual(self.execute('SELECT * FROM t ORDER BY id')['rows'],
                         [[1, 'short'], [3, 'y' * 16000]])

    def test_large_rows_rollback_and_failed_statement(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(20000))')
        old = 'a' * 10000
        self.execute(f"INSERT INTO t VALUES (1,'{old}')")
        self.execute('BEGIN')
        self.execute("UPDATE t SET s='" + 'b' * 19000 + "' WHERE id=1")
        self.execute("INSERT INTO t VALUES (2,'" + 'c' * 12000 + "')")
        self.execute('ROLLBACK')
        failed = self.session.execute("INSERT INTO t VALUES (3,'" + 'd' * 15000 + "'),(1,'dup')")
        self.assertIs(failed.get('ok'), False, failed)
        self.execute("INSERT INTO t VALUES (4,'committed')")
        self.crash_restart()
        self.assertEqual(self.execute('SELECT * FROM t ORDER BY id')['rows'],
                         [[1, old], [4, 'committed']])
        self.execute('BEGIN')
        self.execute('DELETE FROM t WHERE id=1')
        self.execute("INSERT INTO t VALUES (5,'" + 'e' * 17000 + "')")
        self.crash_restart()
        self.assertEqual(self.execute('SELECT * FROM t ORDER BY id')['rows'],
                         [[1, old], [4, 'committed']])

    def test_overflow_reuse_does_not_overwrite_small_rows(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(20000))')
        large = 'z' * 17000
        self.execute(f"INSERT INTO t VALUES (0,'{large}'),(1,'{large}')")
        self.execute('DELETE FROM t WHERE id=1')
        small = 'small' * 10
        self.execute('INSERT INTO t VALUES ' + ','.join(
            f"({i},'{small}')" for i in range(10, 410)))
        self.execute(f"INSERT INTO t VALUES (2,'{large}')")
        # Reclaim normal slots and force compaction of pages containing tagged
        # pointers; both their tag and payload must survive relocation.
        self.execute('DELETE FROM t WHERE id >= 10 AND id < 110')
        self.execute('INSERT INTO t VALUES ' + ','.join(
            f"({i},'{small}')" for i in range(410, 510)))
        expected = [[0, large], [2, large]] + [[i, small] for i in range(110, 510)]
        self.assertEqual(self.execute('SELECT * FROM t ORDER BY id')['rows'], expected)
        self.session.restart()
        self.assertEqual(self.execute('SELECT * FROM t ORDER BY id')['rows'], expected)

    def test_freed_overflow_pages_reused_after_restart(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(20000))')
        self.execute("INSERT INTO t VALUES (0,'keep')")
        large = 'z' * 16000
        baseline = None
        for cycle in range(5):
            self.execute('INSERT INTO t VALUES ' + ','.join(
                f"({i},'{large}')" for i in range(1, 21)))
            self.execute('DELETE FROM t WHERE id >= 1')
            self.session.restart()
            self.assertEqual(self.execute('SELECT * FROM t ORDER BY id')['rows'], [[0, 'keep']])
            size = next(self.session.data_dir.glob('*.heap')).stat().st_size
            if baseline is None:
                baseline = size
            self.assertLessEqual(size, baseline, f'overflow pages leaked on restart cycle {cycle}')

    def test_large_vector_exact_ann_and_restart(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(4096))')
        zero = '[' + ','.join(['0'] * 4096) + ']'
        one = '[' + ','.join(['1'] * 4096) + ']'
        self.execute(f"INSERT INTO t VALUES (2,'{one}'),(1,'{zero}')")
        self.execute('CREATE VECTOR INDEX vi ON t(v)')
        query = f"SELECT id,L2_DISTANCE(v,'{zero}') AS d FROM t ORDER BY d LIMIT 2"
        for mode in ('exact', 'ann'):
            self.assertEqual(self.execute(query, mode)['rows'], [[1, 0], [2, 64]])
        self.session.restart()
        self.assertEqual(self.execute(query)['rows'], [[1, 0], [2, 64]])


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', type=Path, default=BINARY)
    args, rest = parser.parse_known_args()
    BINARY = args.bin.resolve()
    unittest.main(argv=[sys.argv[0]] + rest)
