#!/usr/bin/env python3
"""Diagnostic CPU comparison of two owned benchmark data directories.

Uses one binary to isolate on-disk layout from code changes. This does not
produce a competition score and does not replace the unchanged full benchmark.
Only pass disposable directories previously created by run_fts_comparison.py.
"""
import argparse
import json
import os
from pathlib import Path
import socket
import subprocess
import time

from run_fts_comparison import stop


def cpu_seconds(pid):
    fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
    return (int(fields[11]) + int(fields[12])) / os.sysconf('SC_CLK_TCK')


def execute(args):
    args.output.mkdir(parents=True, exist_ok=False)
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(('127.0.0.1', args.port))
    client = ['taskset', '-c', args.client_cpu, 'mysql', '-h127.0.0.1', '-P' + str(args.port),
              '-uroot', '-N', '-s', '-A', '--default-character-set=utf8mb4']
    queries = {
        'cn': "SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title,content) AGAINST('全文索引 分词器')",
        'beng': "SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title_en,content_en) AGAINST('database performance audit')",
        'mixed': "SELECT COUNT(*) FROM fts_large_bench.docs WHERE MATCH(title,content) AGAINST('OceanBase 稳定 database')",
    }
    results = []
    for number, label in enumerate(('before', 'after', 'after', 'before', 'before', 'after'), 1):
        base = getattr(args, label).resolve(strict=True)
        process = None
        command = ['taskset', '-c', args.server_cpus, str(args.binary.resolve()), '--nodaemon',
                   '--base-dir=' + str(base), '--port=' + str(args.port), '--parameter',
                   'memory_limit=2G', '--parameter', 'cpu_count=2', '--log-level=WARN']
        with (args.output / f'{number}-{label}.server.log').open('w') as log:
            try:
                process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        raise RuntimeError(f'owned server exited: {process.returncode}')
                    if subprocess.run(client + ['-e', 'SELECT 1'], capture_output=True).returncode == 0:
                        break
                    time.sleep(0.2)
                else:
                    raise RuntimeError('owned server readiness timeout')
                for name, query in queries.items():
                    subprocess.run(client, input='SET NAMES utf8mb4;\n' + (query + ';\n') * 30,
                                   text=True, stdout=subprocess.DEVNULL, check=True, timeout=60)
                    begin_cpu, begin_wall = cpu_seconds(process.pid), time.monotonic()
                    subprocess.run(client, input='SET NAMES utf8mb4;\n' + (query + ';\n') * args.rounds,
                                   text=True, stdout=subprocess.DEVNULL, check=True, timeout=180)
                    elapsed_cpu = cpu_seconds(process.pid) - begin_cpu
                    elapsed_wall = time.monotonic() - begin_wall
                    result = {'round': number, 'layout': label, 'query': name, 'queries': args.rounds,
                              'server_cpu_us': elapsed_cpu * 1e6 / args.rounds,
                              'wall_us': elapsed_wall * 1e6 / args.rounds, 'base': str(base)}
                    results.append(result)
                    (args.output / 'results.json').write_text(json.dumps(results, indent=2))
                    print(result, flush=True)
            finally:
                stop(process)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--before', type=Path, required=True)
    parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--port', type=int, default=12890)
    parser.add_argument('--rounds', type=int, default=2000)
    parser.add_argument('--server-cpus', default='8,10')
    parser.add_argument('--client-cpu', default='12')
    execute(parser.parse_args())
