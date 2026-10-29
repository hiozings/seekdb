#!/usr/bin/env python3
"""Dedicated instance: no queue SQL while idle; CREATE/ALTER wakes the worker."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time
from test_ai_recovery import wait_for, stop_owned


def run(args):
    for port in (args.port, args.mock_port):
        with socket.socket() as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            probe.bind(('127.0.0.1', port))
    evidence = Path(tempfile.mkdtemp(prefix='seekdb-ai-idle-', dir=args.output_parent))
    base = evidence / 'database'
    base.mkdir()
    mock_dir = evidence / 'mock'
    mock_dir.mkdir()
    print('Evidence: ' + str(evidence), flush=True)
    logs = []
    server = mock = None
    summary = {'passed': False}

    def sql(query):
        result = subprocess.run(['mysql', '-h127.0.0.1', '-P' + str(args.port), '-uroot', '-N', '-B'],
                                input=query + ';\n', capture_output=True, text=True, timeout=15)
        if result.returncode:
            raise RuntimeError(result.stderr)
        return result.stdout.strip()

    def ready():
        if server.poll() is not None:
            raise RuntimeError('owned server exited')
        try:
            return sql('SELECT 1') == '1'
        except RuntimeError:
            return False

    try:
        log = (evidence / 'server.log').open('w'); logs.append(log)
        command = [str(args.binary.resolve()), '--nodaemon', '--base-dir=' + str(base),
                   '--port=' + str(args.port), '--parameter', 'memory_limit=1G',
                   '--parameter', 'cpu_count=2', '--log-level=INFO']
        server = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        wait_for('fresh database startup', ready, 180)
        # Allow the one-time persisted-queue probe to finish before observation.
        time.sleep(1)
        counter_sql = ("SELECT COALESCE(SUM(EXECUTIONS),0) FROM oceanbase.`V$OB_PLAN_CACHE_PLAN_STAT` "
                       "WHERE QUERY_SQL LIKE 'SELECT * FROM oceanbase.__all_ai_column_task%'")
        before = int(sql(counter_sql))
        time.sleep(2)
        after = int(sql(counter_sql))
        summary['idle_task_sql_before'] = before
        summary['idle_task_sql_after'] = after
        summary['idle_task_sql_executions'] = after - before
        assert after == before, 'worker repeatedly executes task SQL with no AI definitions/tasks'
        log = (evidence / 'mock.log').open('w'); logs.append(log)
        mock = subprocess.Popen([sys.executable, str(Path(__file__).with_name('test_ai_recovery.py')),
                                 '--mock-worker', str(args.mock_port), '--mock-evidence', str(mock_dir)],
                                stdout=log, stderr=subprocess.STDOUT)
        def mock_ready():
            try:
                with socket.create_connection(('127.0.0.1', args.mock_port), timeout=.2):
                    return True
            except OSError:
                return False
        wait_for('mock startup', mock_ready, 10)
        ai_column = "result VARCHAR(300) AI COLUMN (AI_COMPLETE('idle_model',content)) WITH(sync_mode='async')"
        if args.activate_by_alter:
            activation = ("CREATE TABLE idle_probe.docs(id INT PRIMARY KEY,content VARCHAR(200));"
                          "INSERT INTO idle_probe.docs VALUES(1,'idle-new');"
                          "ALTER TABLE idle_probe.docs ADD COLUMN " + ai_column + ";")
        else:
            activation = ("CREATE TABLE idle_probe.docs(id INT PRIMARY KEY,content VARCHAR(200),"
                          + ai_column + ");INSERT INTO idle_probe.docs(id,content) VALUES(1,'idle-new');")
        sql("CREATE DATABASE idle_probe; CALL DBMS_AI_SERVICE.CREATE_AI_MODEL('idle_model',"
            "'{\"type\":\"completion\",\"model_name\":\"idle-text\"}');"
            "CALL DBMS_AI_SERVICE.CREATE_AI_MODEL_ENDPOINT('idle_endpoint',"
            "'{\"ai_model_name\":\"idle_model\",\"url\":\"http://127.0.0.1:"
            + str(args.mock_port) + "/v1/chat/completions\",\"access_key\":\"mock-key\",\"provider\":\"openai\"}');"
            + activation)
        wait_for('AI work after schema change', lambda: sql('SELECT IFNULL(result,\'\') FROM idle_probe.docs') == 'mock:idle-new', 30)
        assert sql('SET SESSION ai_column_strict_read=ON; SELECT result FROM idle_probe.docs') == 'mock:idle-new'
        wait_for('queue drain', lambda: sql('SELECT COUNT(*) FROM oceanbase.__all_ai_column_task') == '0', 10)
        summary.update(passed=True, result='mock:idle-new', remaining_tasks=0, activation='ALTER' if args.activate_by_alter else 'CREATE')
        print('PASS: zero idle task SQL executions; CREATE AI COLUMN wakes worker; strict READY and task drain', flush=True)
    finally:
        stop_owned(server)
        stop_owned(mock)
        for stream in logs:
            stream.close()
        (evidence / 'summary.json').write_text(json.dumps(summary, indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--port', type=int, default=12888)
    parser.add_argument('--mock-port', type=int, default=19130)
    parser.add_argument('--output-parent', default='/tmp')
    parser.add_argument('--activate-by-alter', action='store_true')
    run(parser.parse_args())
