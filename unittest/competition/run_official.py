#!/usr/bin/env python3
"""Run unchanged competition mysqltests on an already-running disposable instance."""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import shutil
import socket
import os
import sys
import subprocess
import tempfile
import threading

REPO = Path(__file__).resolve().parents[2]
SUITES = REPO / 'tools/deploy/mysql_test/test_suite'
CASES = {**{n: 'ai_column' for n in ['ai_column_ddl', 'ai_column_t1_dml',
         'ai_column_worker', 'ai_column_retry_strict', 'ai_column_vector_index']},
         'ai_prompt': 'ai_function', 'ai_function_enhancement': 'ai_function',
         'ai_model_endpoint_ddl': 'ai_function', 'file_sql_basic': 'file_sql'}


def verify_official():
    files = subprocess.check_output(['git', 'diff-tree', '--no-commit-id', '--name-only',
                                     '-r', '5bce9e0'], cwd=REPO, text=True).splitlines()
    if len(files) != 30:
        raise RuntimeError('unexpected official baseline manifest')
    official_cases = {
        Path(name).stem: Path(name).parent.parent.name for name in files
        if '/test_suite/' in name and Path(name).parent.name == 't'
        and Path(name).suffix == '.test'
        and Path(name).parent.parent.name in {'ai_column', 'ai_function', 'file_sql'}
    }
    if CASES != official_cases:
        raise RuntimeError('runner case list differs from official SQL entrypoints')
    hashes = {}
    for name in files:
        original = subprocess.check_output(['git', 'show', '5bce9e0:' + name], cwd=REPO)
        current = (REPO / name).read_bytes()
        if current != original:
            raise RuntimeError('official file differs: ' + name)
        hashes[name] = hashlib.sha256(current).hexdigest()
    expected = ['basic.csv', 'basic.jsonl', 'invalid.txt', 'simple.parquet.b64']
    if sorted(p.name for p in (SUITES / 'file_sql/data').iterdir()) != expected:
        raise RuntimeError('official data directory contains extra or missing files')
    if sorted(p.name for p in (SUITES / 'file_sql/t').glob('*.test')) != ['file_sql_basic.test']:
        raise RuntimeError('official File SQL discovery is contaminated')
    return hashes


def sql(args, query):
    return subprocess.check_output([args.mysql, '--socket=' + args.socket, '-uroot',
                                    '-N', '-B', '-e', query], text=True).rstrip('\n')


def account_sql(args, query):
    # Pass the ephemeral password only via stdin, never process argv or a log.
    result = subprocess.run([args.mysql, '--host=127.0.0.1', '--port=' + str(args.port),
                             '-uroot', '-N', '-B'], input=query + ';\n',
                            capture_output=True, text=True, timeout=15)
    if result.returncode:
        diagnostic = result.stderr
        if "IDENTIFIED BY '" in query:
            password = query.split("IDENTIFIED BY '", 1)[1].split("'", 1)[0]
            diagnostic = diagnostic.replace(password, '<ephemeral-password>')
        raise RuntimeError('endpoint account SQL failed: ' + diagnostic.strip())
    return result.stdout.rstrip('\n')


def redact_credential_files(directory, password):
    for path in directory.iterdir():
        if path.is_file() and path.suffix in ('.log', '.reject', '.txt'):
            content = path.read_text(errors='replace')
            if password in content:
                path.write_text(content.replace(password, '<ephemeral-password>'))


def literal(value):
    return "'" + value.replace('\\', '\\\\').replace("'", "''") + "'"


def run(args):
    hashes = verify_official()
    if args.verify_only:
        print('PASS: 30 official files byte-identical; official discovery/data exact')
        return 0
    work = Path(tempfile.mkdtemp(prefix='seekdb-official-', dir=args.output_parent))
    print('Evidence: ' + str(work), flush=True)
    (work / 'official-sha256.json').write_text(json.dumps(hashes, indent=2))
    overlay = work / 'mysql_test/test_suite/file_sql/include'
    overlay.mkdir(parents=True)
    for name in ['copy_file_sql_data.inc', 'remove_file_sql_data.inc']:
        (overlay / name).write_text('# Local deployment is performed and restored by run_official.py.\n')
    # The only official include override is SSH/admin fixture transport. SQL bodies remain intact.
    ai_include = work / 'mysql_test/test_suite/ai_column/include'
    ai_include.mkdir(parents=True)
    shutil.copyfile(SUITES / 'ai_column/include/ai_column_mock.py', ai_include / 'ai_column_mock.py')
    failures = []
    for name in args.cases or CASES:
        suite = SUITES / CASES[name]
        case_work = work / name
        case_work.mkdir()
        test = suite / 't' / (name + '.test')
        expected = case_work / (name + '.result')
        shutil.copyfile(suite / 'r/mysql' / expected.name, expected)
        driver = case_work / 'driver.test'
        driver.write_text('--disable_query_log\nlet $OBSERVER_DIR=' + str(case_work)
                          + ';\n--enable_query_log\n--source ' + str(test) + '\n')
        data = case_work / 'data_for_mysqltest_file_sql'
        previous_policy = None
        service = None
        thread = None
        mock_port = {'ai_column_t1_dml': 19121, 'ai_column_worker': 19122,
                     'ai_column_vector_index': 19123, 'ai_column_retry_strict': 19124}.get(name)
        mock_owned = False
        accounts = None
        command = [args.mysqltest, '--host=127.0.0.1', '--port=' + str(args.port),
                   '--user=root', '--database=test', '--test-file=' + str(driver),
                   '--result-file=' + str(expected), '--logdir=' + str(case_work)]
        (case_work / 'command.json').write_text(json.dumps(command, indent=2))
        try:
            if name == 'ai_model_endpoint_ddl':
                from endpoint_accounts import EndpointAccounts
                accounts = EndpointAccounts(lambda query: account_sql(args, query))
                accounts.setup()
                # Set credentials only in a mode-0600 temporary driver; delete it in finally.
                driver.chmod(0o600)
                driver.write_text('--disable_query_log\nlet $OBMYSQL_MS0=127.0.0.1;\n'
                                  + 'let $OBMYSQL_PORT=' + str(args.port) + ';\n'
                                  + 'let $OBMYSQL_PWD=' + accounts.password + ';\n'
                                  + '--enable_query_log\n--source ' + str(test) + '\n')
                (case_work / 'account-fixture.txt').write_text(
                    'Owned admin@127.0.0.1; CREATE USER without GRANT OPTION; SELECT on test.* '
                    'and CREATE/ALTER/DROP AI MODEL on *.* with GRANT OPTION; mysql.user SELECT without GRANT OPTION.\n'
                    'Existing admin/test_ai_user on any host causes refusal. No global settings changed.\n')
            if mock_port is not None:
                with socket.socket() as probe:
                    probe.bind(('127.0.0.1', mock_port))
                pid_path = Path(f'/tmp/seekdb_ai_column_mock_{mock_port}/server.pid')
                if pid_path.exists():
                    try:
                        os.kill(int(pid_path.read_text()), 0)
                    except ProcessLookupError:
                        pid_path.unlink()
                    else:
                        raise RuntimeError('official mock pid already exists: ' + str(pid_path))
                mock_owned = True
            if name == 'file_sql_basic':
                data.mkdir()
                for source in (suite / 'data').iterdir():
                    if source.suffix == '.b64':
                        (data / 'simple.parquet').write_bytes(base64.b64decode(source.read_bytes(), validate=False))
                    else:
                        shutil.copyfile(source, data / source.name)
                (case_work / 'deployed-files.json').write_text(json.dumps(sorted(p.name for p in data.iterdir())))
                previous_policy = sql(args, 'SELECT @@GLOBAL.secure_file_priv')
                sql(args, 'SET GLOBAL secure_file_priv=' + literal(str(data)))
            if name == 'ai_function_enhancement':
                from official_ai_fixture import Service
                service = Service()
                thread = threading.Thread(target=service.serve_forever)
                thread.start()
            if accounts is not None:
                accounts.case_started = True
            with (case_work / 'mysqltest.log').open('w') as log:
                result = subprocess.run(command, cwd=work, stdout=log, stderr=subprocess.STDOUT,
                                        timeout=args.timeout)
            if result.returncode:
                failures.append(name)
            print(name + (': PASS' if result.returncode == 0 else ': FAIL'), flush=True)
        except Exception as error:
            failures.append(name)
            (case_work / 'runner-error.txt').write_text(repr(error))
            print(name + ': RUNNER ERROR: ' + str(error), flush=True)
        finally:
            if accounts is not None:
                try:
                    accounts.cleanup()
                except Exception as error:
                    if name not in failures:
                        failures.append(name)
                    (case_work / 'cleanup-error.txt').write_text(str(error))
                    print(name + ': ACCOUNT CLEANUP FAILED', flush=True)
                finally:
                    driver.unlink(missing_ok=True)
                    redact_credential_files(case_work, accounts.password)
            if mock_owned:
                state = Path(f'/tmp/seekdb_ai_column_mock_{mock_port}')
                if state.exists():
                    shutil.copytree(state, case_work / 'mock-state')
                subprocess.run([sys.executable, str(ai_include / 'ai_column_mock.py'),
                                'stop', str(mock_port)], check=True)
            if service is not None:
                service.shutdown()
                thread.join()
                service.server_close()
                (case_work / 'mock-requests.json').write_text(json.dumps(service.requests, indent=2))
            if previous_policy is not None:
                sql(args, 'SET GLOBAL secure_file_priv=' + literal(previous_policy))
            if data.exists():
                shutil.rmtree(data)
    verify_official()
    (work / 'summary.json').write_text(json.dumps({'failed': failures, 'cases': args.cases or list(CASES)}, indent=2))
    return bool(failures)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify-only', action='store_true')
    parser.add_argument('--port', type=int, default=12883)
    parser.add_argument('--socket', default='/tmp/seekdb-file-sql-instance/run/sql.sock')
    parser.add_argument('--mysql', default='mysql')
    parser.add_argument('--mysqltest', default=str(REPO / 'deps/3rd/u01/obclient/bin/mysqltest'))
    parser.add_argument('--output-parent', default='/tmp')
    parser.add_argument('--timeout', type=int, default=900)
    # Python 3.9 argparse validates the empty nargs='*' list against choices.
    # Validate the optional case names after parsing so the default full run works.
    parser.add_argument('cases', nargs='*', metavar='CASE')
    arguments = parser.parse_args()
    unknown = sorted(set(arguments.cases) - set(CASES))
    if unknown:
        parser.error('unknown cases: ' + ', '.join(unknown))
    raise SystemExit(run(arguments))
