#!/usr/bin/env python3
"""Run unchanged official FTS benchmarks in an owned disposable instance.
Alternates baseline/candidate with restart and full load on each run; never attaches
or signals an existing server. CPU affinity is fixed for both server and clients.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import time

REPO = Path(__file__).resolve().parents[3]


def stop(process):
    if process is not None and process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=30)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=10)


def execute(args):
    with socket.socket() as probe:
        # A stopped owned server can leave TIME_WAIT sockets; an active
        # listener still conflicts because SO_REUSEPORT is never enabled.
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(('127.0.0.1', args.port))
    args.output.mkdir(parents=True, exist_ok=False)
    base = args.output / 'instance'
    base.mkdir()
    hashes = {}
    for name, path in [('baseline', args.baseline), ('candidate', args.candidate)]:
        digest = hashlib.sha256()
        with path.open('rb') as binary:
            for chunk in iter(lambda: binary.read(1024 * 1024), b''):
                digest.update(chunk)
        hashes[name] = digest.hexdigest()
    (args.output / 'binaries.json').write_text(json.dumps(hashes, indent=2))
    mysql = ['taskset', '-c', args.client_cpu, 'mysql', '-h127.0.0.1', '-P' + str(args.port), '-uroot', '--default-character-set=utf8mb4']
    order = ['baseline', 'candidate', 'candidate', 'baseline', 'baseline', 'candidate']
    summaries = []
    for index, name in enumerate(order):
        label = f'{index + 1}-{name}'
        binary = getattr(args, name)
        server_command = ['taskset', '-c', args.server_cpus, str(binary), '--nodaemon',
                          '--base-dir=' + str(base), '--port=' + str(args.port),
                          '--parameter', 'memory_limit=2G', '--parameter', 'cpu_count=2', '--log-level=WARN']
        process = None
        with (args.output / (label + '.server.log')).open('w') as server_log:
            try:
                process = subprocess.Popen(server_command, cwd=base, stdout=server_log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError('owned server exited before readiness')
                    ready = subprocess.run(mysql + ['-N', '-e', 'SELECT 1'], capture_output=True, timeout=10)
                    if ready.returncode == 0:
                        break
                    time.sleep(0.2)
                else:
                    raise RuntimeError('owned server readiness timeout')
                env = os.environ.copy()
                env.update(MYSQL=' '.join(mysql + ['-N', '-s', '-A']), MYSQL_VERBOSE=' '.join(mysql + ['-A']),
                           LABEL=label, ROWS='20000', BATCH='500', ROUNDS='3000', QUERY_ROUNDS='200',
                           SAMPLES='3', WARMUP='30', SKIP_LOAD='0', OUTPUT=str(args.output / (label + '.report')))
                with (args.output / (label + '.bench.log')).open('w') as log:
                    subprocess.run(['bash', str(REPO / 'tools/benchmark/fts_large_bench.sh')],
                                   cwd=REPO, env=env, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=900)
                summaries.append({'label': label, 'binary': str(binary), 'sha256': hashes[name],
                                  'server_command': server_command, 'report': label + '.report'})
                (args.output / 'runs.json').write_text(json.dumps(summaries, indent=2))
                print(label + ': complete', flush=True)
            finally:
                stop(process)
    print('PASS: six full benchmark runs; owned server stopped', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True)
    parser.add_argument('--candidate', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--port', type=int, default=12886)
    parser.add_argument('--server-cpus', default='8,10')
    parser.add_argument('--client-cpu', default='12')
    execute(parser.parse_args())
