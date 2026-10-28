#!/usr/bin/env python3
"""Crash/restart regression owning its database, mock and persistent data directory.

Run inside an isolated test container after coordinating memory with other tests.
Only a Popen-owned server is signalled. No existing database is contacted.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import types

REPO = Path(__file__).resolve().parents[2]
MOCK = REPO / 'tools/deploy/mysql_test/test_suite/ai_column/include/ai_column_mock.py'


def wait_for(description, predicate, seconds=120):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.2)
    raise AssertionError('timed out: ' + description)


def stop_owned(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


def serve_mock(port, evidence):
    """Retain official responses; instrument only delay and state directory."""
    spec = importlib.util.spec_from_file_location('official_mock', MOCK)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.state_dir = lambda ignored: str(evidence)
    lock = threading.Lock()
    starts = 0

    def controlled_sleep(seconds):
        nonlocal starts
        if seconds == 1.2:  # official slow-old fault point, no response changes
            with lock:
                starts += 1
                attempt = starts
                with (evidence / 'request-starts.jsonl').open('a') as stream:
                    stream.write(json.dumps({'attempt': attempt, 'time': time.time()}) + '\n')
            if attempt == 1:
                wait_for('parent releases first response after database SIGKILL',
                         lambda: (evidence / 'release-response').exists(), 45)
            else:
                time.sleep(0.05)
        else:
            time.sleep(seconds)

    module.time = types.SimpleNamespace(sleep=controlled_sleep)
    module.serve(port)


def run(args):
    for port in [args.port, args.mock_port]:
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', port))
    evidence = Path(tempfile.mkdtemp(prefix='seekdb-ai-recovery-', dir=args.output_parent))
    base = evidence / 'database'
    base.mkdir()
    mock_dir = evidence / 'mock'
    mock_dir.mkdir()
    print('Evidence: ' + str(evidence), flush=True)
    binary = Path(args.binary).resolve(strict=True)
    command = [str(binary), '--nodaemon', '--base-dir=' + str(base), '--port=' + str(args.port),
               '--parameter', 'memory_limit=' + args.memory_limit,
               '--parameter', 'cpu_count=2', '--log-level=WARN']
    (evidence / 'server-command.json').write_text(json.dumps(command, indent=2))
    (evidence / 'fault-injection.txt').write_text(
        'Dedicated disposable instance only. SQL sets RUNNING task lease_until=0 before SIGKILL.\n'
        'This accelerates lease expiry; it does not modify result/state/owner/request or eligibility.\n'
        'Official mock responses unchanged; its slow-old sleep is gated until after SIGKILL.\n'
        'Mock state directory redirected inside evidence. First response is lost; second must complete.\n')
    server = mock = None
    logs = []
    summary = {'passed': False}

    def sql(query):
        result = subprocess.run([args.mysql, '--host=127.0.0.1', '--port=' + str(args.port),
                                 '-uroot', '-N', '-B', '--raw', '-e', query],
                                capture_output=True, text=True, timeout=10)
        with (evidence / 'sql-transcript.jsonl').open('a') as stream:
            stream.write(json.dumps({'sql': query, 'returncode': result.returncode,
                                     'stdout': result.stdout, 'stderr': result.stderr}) + '\n')
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.strip()

    def start_server(label):
        log = (evidence / (label + '.log')).open('w')
        logs.append(log)
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        return process

    def ready():
        if server.poll() is not None:
            raise RuntimeError('owned server exited: ' + str(server.returncode))
        try:
            return sql('SELECT 1') == '1'
        except (RuntimeError, subprocess.TimeoutExpired):
            return False

    try:
        log = (evidence / 'mock.log').open('w')
        logs.append(log)
        mock = subprocess.Popen([sys.executable, __file__, '--mock-worker', str(args.mock_port),
                                 '--mock-evidence', str(mock_dir)], stdout=log, stderr=subprocess.STDOUT)
        def mock_ready():
            if mock.poll() is not None:
                raise RuntimeError('owned mock exited')
            try:
                with socket.create_connection(('127.0.0.1', args.mock_port), timeout=0.2):
                    return True
            except OSError:
                return False
        wait_for('mock startup', mock_ready, 10)
        server = start_server('first-start')
        wait_for('fresh database startup', ready, 180)
        sql("CREATE DATABASE recovery; CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('recovery_model', "
            "'{\"type\":\"completion\",\"model_name\":\"recovery-complete\"}'); "
            "CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('recovery_endpoint', "
            "'{\"ai_model_name\":\"recovery_model\",\"url\":\"http://127.0.0.1:"
            + str(args.mock_port) + "/v1/chat/completions\",\"access_key\":\"mock-key\",\"provider\":\"openai\"}'); "
            "CREATE TABLE recovery.docs(id BIGINT PRIMARY KEY, content VARCHAR(200), "
            "result VARCHAR(500) AI COLUMN (AI_COMPLETE('recovery_model', content)) "
            "WITH (sync_mode='async', max_retries=3));")
        sql("START TRANSACTION; INSERT INTO recovery.docs(id,content) VALUES(2,'rollback-value'); "
            "ROLLBACK; SELECT COUNT(*) FROM recovery.docs;")
        sql("INSERT INTO recovery.docs(id,content) VALUES(1,'slow-old');")
        wait_for('model request entered controlled delay', lambda: (mock_dir / 'request-starts.jsonl').exists(), 30)
        assert sql('SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE state=1') == '1'
        before = sql('SELECT table_id,column_id,request_id,state,owner_token,lease_until,retry_count '
                     'FROM oceanbase.__all_ai_column_task')
        (evidence / 'task-before-crash.tsv').write_text(before + '\n')
        sql('UPDATE oceanbase.__all_ai_column_task SET lease_until=0 WHERE state=1')
        assert sql('SELECT COUNT(*) FROM oceanbase.__all_ai_column_task WHERE state=1 AND lease_until=0') == '1'
        server.kill()  # Popen owns this exact PID; never signal an existing instance.
        server.wait(timeout=10)
        summary['killed_pid'] = server.pid
        (mock_dir / 'release-response').touch()
        wait_for('first request completed after server death', lambda: (mock_dir / 'events.jsonl').exists(), 10)
        server = start_server('restart')
        wait_for('same directory recovery startup', ready, 180)
        wait_for('recovered result', lambda: sql('SELECT IFNULL(result,\'\') FROM recovery.docs WHERE id=1')
                 == 'mock:slow-old', 60)
        assert sql('SET SESSION ai_column_strict_read=ON; SELECT result FROM recovery.docs WHERE id=1') == 'mock:slow-old'
        assert sql('SELECT COUNT(*) FROM oceanbase.__all_ai_column_task') == '0'
        assert sql('SELECT COUNT(*) FROM recovery.docs WHERE id=2') == '0'
        events = [json.loads(line) for line in (mock_dir / 'events.jsonl').read_text().splitlines()]
        starts = [json.loads(line) for line in (mock_dir / 'request-starts.jsonl').read_text().splitlines()]
        assert len(events) == len(starts) == 2, (events, starts)
        assert all(e['user'] == 'slow-old' and e['status'] == 200 for e in events), events
        summary.update(passed=True, request_count=len(events), duplicate_calls=1,
                       response_loss='first response after SIGKILL; retry after restart',
                       strict_ready_result='mock:slow-old', remaining_tasks=0, rollback_requests=0)
        print('PASS: same-directory recovery, strict READY read, task drain, 2 calls/1 duplicate, rollback silent', flush=True)
    finally:
        stop_owned(server)
        stop_owned(mock)
        for log in logs:
            log.close()
        (evidence / 'summary.json').write_text(json.dumps(summary, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default=str(REPO / 'build_release/src/observer/seekdb'))
    parser.add_argument('--mysql', default='mysql')
    parser.add_argument('--port', type=int, default=12885)
    parser.add_argument('--mock-port', type=int, default=19129)
    parser.add_argument('--memory-limit', choices=['1G', '2G'], default='1G')
    parser.add_argument('--output-parent', default='/tmp')
    parser.add_argument('--mock-worker', type=int, help=argparse.SUPPRESS)
    parser.add_argument('--mock-evidence', type=Path, help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.mock_worker is not None:
        serve_mock(args.mock_worker, args.mock_evidence)
    else:
        run(args)
