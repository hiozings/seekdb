#!/usr/bin/env python3
"""SQL and JSONL edge cases, exercised through the production protocol."""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol_client import Session
BINARY = Path(__file__).resolve().parents[2] / 'build/minivectordb'

class SqlProtocolEdges(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory(prefix='mvdb-protocol-edges-')
        self.addCleanup(directory.cleanup)
        self.session = Session(BINARY, Path(directory.name), ["--stats"])
        self.addCleanup(self.session.close)

    def execute(self, sql):
        response = self.session.execute(sql)
        self.assertIs(response.get('ok'), True, response)
        return response

    def raw(self, line):
        self.session.proc.stdin.write(line + '\n')
        self.session.proc.stdin.flush()
        return json.loads(self.session.proc.stdout.readline())

    def test_malformed_json_numbers_cannot_execute(self):
        invalid = ['-', '1e', '1..2', '01', '-01', '1.', '1e+', '1e-', '1+2']
        for i, number in enumerate(invalid):
            with self.subTest(number=number):
                sql = f'CREATE TABLE malformed_{i} (id BIGINT PRIMARY KEY)'
                request = '{"id":' + number + ',"op":"execute","sql":' + json.dumps(sql) + '}'
                response = self.raw(request)
                self.assertIs(response.get('ok'), False, response)
                self.assertEqual(response['error']['code'], 'syntax_error')
                self.execute(sql)

    def test_valid_json_number_ids_preserve_value(self):
        for number in ['0', '-0', '1.25', '-1.25', '1e2', '1E+2', '1e-2', '9223372036854775807']:
            with self.subTest(number=number):
                response = self.raw('{"id":' + number + ',"op":"execute","sql":"SELECT 1 AS n"}')
                self.assertIs(response.get('ok'), True, response)
                self.assertEqual(response['id'], json.loads(number))

    def test_request_ids_preserve_json_types_and_escaping(self):
        for ident in ['hello', 'quote"\\line\n😀', True, None, ['nested', 42], {'key': 'value'}]:
            with self.subTest(ident=ident):
                response = self.raw(json.dumps({'id': ident, 'op': 'execute', 'sql': 'SELECT 1 AS n'}))
                self.assertIs(response.get('ok'), True, response)
                self.assertEqual(response['id'], ident)
                self.assertIs(type(response['id']), type(ident))

    def test_unescaped_json_controls_cannot_execute(self):
        for i, control in enumerate(['\t', '\x00', '\x1f']):
            with self.subTest(control=control):
                sql = f'CREATE TABLE control_{i} (id BIGINT PRIMARY KEY)'
                response = self.raw('{"id":"' + control + '","op":"execute","sql":' + json.dumps(sql) + '}')
                self.assertIs(response.get('ok'), False, response)
                self.execute(sql)

    def test_explicit_empty_insert_columns_rejected_atomically(self):
        self.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1))')
        self.execute('BEGIN')
        self.execute("INSERT INTO t VALUES(1,'[1]')")
        response=self.session.execute("INSERT INTO t () VALUES(2,'[0]')")
        self.assertIs(response.get('ok'),False,response)
        self.execute('COMMIT')
        self.session.restart()
        self.assertEqual(self.execute('SELECT id FROM t ORDER BY id')['rows'],[[1]])

    def test_unsupported_vector_comparisons_rejected_before_scanning(self):
        self.execute('CREATE TABLE t(id BIGINT PRIMARY KEY,v VECTOR(1))')
        for count in range(3):
            for sql in ("SELECT id FROM t WHERE v='[1]'", 'SELECT id FROM t ORDER BY v',
                        "SELECT id FROM t WHERE v>'[0]' ORDER BY L2_DISTANCE(v,'[0]') LIMIT 1",
                        "UPDATE t SET v='[2]' WHERE v='[1]'", "DELETE FROM t WHERE v='[1]'"):
                with self.subTest(rows=count,sql=sql):
                    response=self.session.execute(sql)
                    self.assertIs(response.get('ok'),False,response)
                    self.assertEqual(response['error']['code'],'unsupported_sql')
            self.assertEqual(self.execute('SELECT * FROM t ORDER BY id')['rows'],[[i,[1]] for i in range(count)])
            self.execute(f"INSERT INTO t VALUES({count},'[1]')")

    def test_driver_environment_override_reaches_child(self):
        import subprocess
        driver=Path(__file__).with_name('driver.py')
        result=subprocess.run([sys.executable,str(driver),'--bin',str(BINARY),'--filter','m0_basic',
            '--env','MVDB_FORMAT=text','--env','MVDB_READY=env-override-sentinel'],
            capture_output=True,text=True,timeout=20)
        self.assertNotEqual(result.returncode,0,'driver silently ignored --env')
        self.assertIn('ready line is not JSON',result.stdout)

    def test_literal_projection_preserves_json_types(self):
        self.assertEqual(self.execute("SELECT 1 AS n, 1.5 AS f, '1' AS s")['rows'], [[1,1.5,'1']])

    def test_unsupported_session_settings_are_not_silently_ignored(self):
        for sql in ('SET autocommit=0', "SET SESSION sql_mode='anything'", 'USE imaginary'):
            response = self.session.execute(sql)
            self.assertIs(response.get('ok'), False, response)
            self.assertEqual(response['error']['code'], 'unsupported_sql')

    def test_json_surrogate_pair_matches_raw_unicode(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, s VARCHAR(1))')
        response = self.raw(json.dumps({'id': 100, 'op': 'execute', 'sql': "INSERT INTO t VALUES (1,'😀')"}))
        self.assertIs(response.get('ok'), True, response)
        self.assertEqual(self.execute("SELECT s FROM t WHERE s='😀'")['rows'], [['😀']])

    def test_hidden_order_alias_cannot_shadow_expression(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(1))')
        self.execute("INSERT INTO t VALUES (1,'[9]'),(2,'[1]')")
        self.assertEqual(self.execute("SELECT id AS __order_distance_1 FROM t ORDER BY L2_DISTANCE(v,'[0]')")['rows'], [[2],[1]])

    def test_invalid_search_mode_does_not_execute(self):
        for mode in ['bogus', 1, None]:
            response = self.session._send({'id': 100, 'op': 'execute', 'sql': 'CREATE TABLE t (id BIGINT PRIMARY KEY)', 'search_mode': mode})
            self.assertIs(response.get('ok'), False, response)
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY)')

    def test_omitted_search_mode_defaults_to_exact(self):
        self.execute('CREATE TABLE t (id BIGINT PRIMARY KEY, v VECTOR(1))')
        self.execute('INSERT INTO t VALUES ' + ','.join(f"({i},'[0]')" for i in range(300,0,-1)))
        self.execute('CREATE VECTOR INDEX vi ON t(v)')
        query = "SELECT id,L2_DISTANCE(v,'[0]') AS d FROM t ORDER BY d LIMIT 3"
        self.session.execute(query, mode='ann')
        response = self.session._send({'id': 100, 'op': 'execute', 'sql': query})
        self.assertEqual(response['rows'], [[1,0],[2,0],[3,0]])
        self.session.close()
        self.assertRegex(self.session.last_stderr, r"\bann_queries=1\b")

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--bin', type=Path, default=BINARY)
    args, rest = parser.parse_known_args()
    BINARY = args.bin.resolve()
    unittest.main(argv=[sys.argv[0], *rest])
