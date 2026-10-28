"""Offline checks of the runner's evidence and failure propagation."""
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import types
import unittest

import endpoint_accounts
import official_ai_fixture
import run_official


class RunnerTests(unittest.TestCase):
    def test_official_manifest_and_discovery(self):
        self.assertEqual(30, len(run_official.verify_official()))

    def test_default_cases_parse_on_supported_python(self):
        result = subprocess.run([sys.executable, run_official.__file__, '--verify-only'],
                                capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stderr)
        invalid = subprocess.run([sys.executable, run_official.__file__, 'unknown_case'],
                                 capture_output=True, text=True)
        self.assertEqual(2, invalid.returncode)
        self.assertIn('unknown cases', invalid.stderr)

    def test_failure_is_not_masked_and_expected_is_copied(self):
        with tempfile.TemporaryDirectory() as directory:
            result = subprocess.run([sys.executable, str(Path(run_official.__file__)),
                                     '--output-parent', directory, '--mysqltest', '/bin/false',
                                     'ai_prompt'], capture_output=True, text=True)
            self.assertNotEqual(0, result.returncode)
            work, = Path(directory).iterdir()
            self.assertEqual(['ai_prompt'], json.loads((work / 'summary.json').read_text())['failed'])
            self.assertEqual((run_official.SUITES / 'ai_function/r/mysql/ai_prompt.result').read_bytes(),
                             (work / 'ai_prompt/ai_prompt.result').read_bytes())
            command = json.loads((work / 'ai_prompt/command.json').read_text())
            self.assertNotIn('--record', command)
            self.assertIn('--result-file=' + str(work / 'ai_prompt/ai_prompt.result'), command)

    def request(self, path, body):
        data = json.dumps(body).encode()
        handler = official_ai_fixture.Handler.__new__(official_ai_fixture.Handler)
        handler.path = path
        handler.headers = {'Content-Length': str(len(data))}
        handler.rfile = io.BytesIO(data)
        handler.server = types.SimpleNamespace(lock=threading.Lock(), requests=[])
        replies = []
        handler.reply = lambda status, value: replies.append((status, value))
        handler.do_POST()
        return replies[0]

    def test_embedding_default_explicit_dimension_and_response_order(self):
        for dim in [None, 3]:
            body = {'input': ['first', 'third']}
            if dim is not None:
                body['dimensions'] = dim
            status, reply = self.request('/v1/embeddings', body)
            self.assertEqual(200, status)
            self.assertEqual([1, 0], [row['index'] for row in reply['data']])
            self.assertEqual([dim or 768] * 2, [len(row['embedding']) for row in reply['data']])

    def test_rerank_scores_depend_on_query(self):
        status, reply = self.request('/v1/rerank', {'query': 'apple', 'documents': ['apple', 'banana']})
        self.assertEqual(200, status)
        self.assertEqual([{'index': 1, 'relevance_score': 0}, {'index': 0, 'relevance_score': 1}], reply['results'])


class EndpointAccountTests(unittest.TestCase):
    def test_existing_account_refused_without_mutation(self):
        calls = []
        def execute(query):
            calls.append(query)
            return '1'
        accounts = endpoint_accounts.EndpointAccounts(execute)
        with self.assertRaisesRegex(RuntimeError, 'already exists'):
            accounts.setup()
        accounts.cleanup()
        self.assertEqual(1, len(calls))
        self.assertTrue(calls[0].startswith('SELECT COUNT'))

    def test_partial_setup_failure_cleans_only_owned_admin(self):
        calls = []
        def execute(query):
            calls.append(query)
            if query.startswith('GRANT SELECT'):
                raise RuntimeError('injected setup failure')
            return '0'
        accounts = endpoint_accounts.EndpointAccounts(execute)
        with self.assertRaisesRegex(RuntimeError, 'injected setup failure'):
            accounts.setup()
        accounts.cleanup()
        self.assertEqual("DROP USER IF EXISTS 'admin'@'127.0.0.1'", calls[-1])
        self.assertFalse(any(q.startswith('DROP') and 'test_ai_user' in q for q in calls))

    def test_grants_and_failed_case_cleanup(self):
        calls = []
        def execute(query):
            calls.append(query)
            return '0'
        accounts = endpoint_accounts.EndpointAccounts(execute)
        accounts.setup()
        self.assertEqual([
            "GRANT CREATE USER ON *.* TO 'admin'@'127.0.0.1'",
            "GRANT SELECT ON test.* TO 'admin'@'127.0.0.1' WITH GRANT OPTION",
            "GRANT SELECT ON mysql.user TO 'admin'@'127.0.0.1'",
            *["GRANT " + p + " ON *.* TO 'admin'@'127.0.0.1' WITH GRANT OPTION"
              for p in ['CREATE AI MODEL', 'ALTER AI MODEL', 'DROP AI MODEL']]],
            [q for q in calls if q.startswith('GRANT')])
        accounts.case_started = True
        accounts.cleanup()
        self.assertEqual(["DROP USER IF EXISTS 'test_ai_user'@'%'",
                          "DROP USER IF EXISTS 'admin'@'127.0.0.1'"], calls[-2:])

    def test_admin_cleanup_attempted_after_test_user_cleanup_error(self):
        calls = []
        def execute(query):
            calls.append(query)
            if query == "DROP USER IF EXISTS 'test_ai_user'@'%'":
                raise RuntimeError('injected cleanup failure')
            return '0'
        accounts = endpoint_accounts.EndpointAccounts(execute)
        accounts.setup()
        accounts.case_started = True
        with self.assertRaisesRegex(RuntimeError, 'cleanup failed'):
            accounts.cleanup()
        self.assertEqual("DROP USER IF EXISTS 'admin'@'127.0.0.1'", calls[-1])


if __name__ == '__main__':
    unittest.main()
