#!/usr/bin/env python3
"""Compare full FTS outputs across two binaries using the SAME owned index.

Run --phase baseline first, then --phase candidate with the other binary and
the same output directory. Both phases stop only their own child process.
This is a correctness run, not a performance measurement.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import socket
import shutil
import subprocess
import time

from compare_fts_results import collect, run
from run_fts_comparison import REPO, stop


def execute(args):
    with socket.socket() as probe:
        probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        probe.bind(('127.0.0.1', args.port))
    if args.phase == 'baseline':
        args.output.mkdir(parents=True, exist_ok=False)
        (args.output / 'instance').mkdir()
    elif not (args.output / 'baseline.json').is_file():
        raise RuntimeError('baseline phase must complete first')
    binary = args.binary.resolve(strict=True)
    digest = hashlib.sha256()
    with binary.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    command = [str(binary), '--nodaemon', '--base-dir=' + str(args.output / 'instance'),
               '--port=' + str(args.port), '--parameter', 'memory_limit=2G',
               '--parameter', 'cpu_count=' + str(args.cpu_count), '--log-level=WARN']
    if args.server_cpus:
        command = ['taskset', '-c', args.server_cpus] + command
    client = ['mysql', '-h127.0.0.1', '-P' + str(args.port), '-uroot',
              '--default-character-set=utf8mb4', '-N', '-s', '-A']
    if args.client_cpu:
        client = ['taskset', '-c', args.client_cpu] + client
    process = None
    with (args.output / (args.phase + '.server.log')).open('w') as log:
        try:
            process = subprocess.Popen(command, cwd=args.output, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 180
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    raise RuntimeError(f'owned server exited before readiness: returncode={process.returncode}')
                if subprocess.run(client + ['-e', 'SELECT 1'], capture_output=True, timeout=10).returncode == 0:
                    break
                time.sleep(0.2)
            else:
                raise RuntimeError('owned server readiness timeout')
            if args.expect_score_free:
                match = "MATCH(title,content) AGAINST('全文索引 分词器')"
                membership = run(args.port, f'EXPLAIN SELECT COUNT(*) FROM fts_large_bench.docs WHERE {match}')
                assert membership['returncode'] == 0, membership
                assert 'calc_relevance=false' in membership['output'], membership['output']
                scored = run(args.port, f'EXPLAIN SELECT id,{match} FROM fts_large_bench.docs '
                                       f'WHERE {match} ORDER BY 2 DESC LIMIT 20')
                assert scored['returncode'] == 0, scored
                assert 'calc_relevance=true' in scored['output'], scored['output']
                limited = run(args.port, "EXPLAIN SELECT COUNT(*) FROM (SELECT id FROM "
                              "fts_large_bench.docs WHERE MATCH(content) "
                              "AGAINST('倒排索引 tokenizer') LIMIT 20) v")
                (args.output / (args.phase + '.plans.json')).write_text(json.dumps(
                    {'membership': membership, 'scored': scored, 'limited_count': limited},
                    ensure_ascii=False, indent=2))
                if args.expect_pruned_limit:
                    assert limited['returncode'] == 0, limited
                    assert 'sort_keys(' not in limited['output'], limited['output']
                    assert 'calc_relevance=false' in limited['output'], limited['output']
                    assert 'is_index_back=false' in limited['output'], limited['output']
            if args.phase == 'baseline':
                env = os.environ.copy()
                env.update(MYSQL=' '.join(client), MYSQL_VERBOSE=' '.join(client),
                           ROWS='20000', BATCH='500', ROUNDS='3000', QUERY_ROUNDS='200',
                           SAMPLES='3', WARMUP='30', SKIP_LOAD='0')
                with (args.output / 'setup.log').open('w') as setup_log:
                    subprocess.run(['bash', str(REPO / 'tools/benchmark/fts_large_bench.sh')],
                                   env=env, stdout=setup_log, stderr=subprocess.STDOUT,
                                   check=True, timeout=900)
            observed = {} if args.skip_collection else collect(args.port)
            if args.build_batches:
                from check_fts_build_batches import collect_build_batches
                observed.update({'build-batch/' + key: value for key, value in collect_build_batches(args.port).items()})
            if args.parallel_builds:
                from check_fts_parallel_builds import collect_parallel_builds
                observed.update({'parallel-build/' + key: value for key, value in collect_parallel_builds(args.port).items()})
            if args.mysqltest is not None:
                suite = REPO / 'tools/deploy/mysql_test/test_suite/fts_index'
                case_dir = args.output / (args.phase + '-mysqltest')
                case_dir.mkdir(exist_ok=False)
                expected = case_dir / 'tokenize_function.result'
                shutil.copyfile(suite / 'r/mysql/tokenize_function.result', expected)
                with (case_dir / 'mysqltest.log').open('w') as test_log:
                    subprocess.run([str(args.mysqltest), '--host=127.0.0.1',
                                    '--port=' + str(args.port), '--user=root', '--database=test',
                                    '--test-file=' + str(suite / 't/tokenize_function.test'),
                                    '--result-file=' + str(expected), '--logdir=' + str(case_dir)],
                                   stdout=test_log, stderr=subprocess.STDOUT, check=True, timeout=300)
            if args.competition:
                with (args.output / (args.phase + '.competition.log')).open('w') as test_log:
                    subprocess.run(['python3', str(REPO / 'unittest/competition/run_official.py'),
                                    '--port', str(args.port), '--socket',
                                    str(args.output / 'instance/run/sql.sock'), '--timeout', '180'],
                                   cwd=REPO, stdout=test_log, stderr=subprocess.STDOUT,
                                   check=True, timeout=1800)
            (args.output / (args.phase + '.json')).write_text(
                json.dumps(observed, ensure_ascii=False, indent=2) + '\n')
            (args.output / (args.phase + '.binary.json')).write_text(json.dumps(
                {'binary': str(binary), 'sha256': digest.hexdigest(), 'command': command}, indent=2))
            if args.phase == 'candidate':
                expected = json.loads((args.output / 'baseline.json').read_text())
                differing = [key for key in sorted(set(expected) | set(observed))
                             if expected.get(key) != observed.get(key)]
                if differing:
                    raise RuntimeError('differing outputs: ' + ', '.join(differing))
            print(f'PASS: {args.phase}: {len(observed)} full token/query/DML outputs', flush=True)
        finally:
            stop(process)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--phase', choices=['baseline', 'candidate'], required=True)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--output', type=lambda value: Path(value).resolve(), required=True)
    parser.add_argument('--port', type=int, default=12889)
    parser.add_argument('--expect-score-free', action='store_true',
                        help='Assert membership plans omit BM25 while scoring plans retain it')
    parser.add_argument('--mysqltest', type=Path, help='Also run the unchanged TOKENIZE SQL regression')
    parser.add_argument('--expect-pruned-limit', action='store_true')
    parser.add_argument('--competition', action='store_true', help='Run unchanged official competition SQL suites')
    parser.add_argument('--cpu-count', type=int, default=2)
    parser.add_argument('--parallel-builds', action='store_true', help='Exercise default and explicit parallel FTS builds')
    parser.add_argument('--server-cpus')
    parser.add_argument('--client-cpu')
    parser.add_argument('--skip-collection', action='store_true', help='Diagnostic build/benchmark only; not correctness evidence')
    parser.add_argument('--build-batches', action='store_true', help='Check offline FTS builds across document and batch boundaries')
    execute(parser.parse_args())
