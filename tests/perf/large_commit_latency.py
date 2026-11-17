#!/usr/bin/env python3
"""Measure large-request latency in a fresh 2-CPU/2-GiB Docker database.

Default data is larger than RAM. Use a disk-backed --data-root, not tmpfs.
This opt-in test creates and removes its own data and container; it is not in
CTest because the full workload writes several GiB and takes a few minutes.
"""
import argparse
import json
from pathlib import Path
import selectors
import subprocess
import tempfile
import time
import uuid


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--image', required=True)
    parser.add_argument('--rows', type=int, default=400000)
    parser.add_argument('--width', type=int, default=6000)
    parser.add_argument('--batch', type=int, default=100)
    parser.add_argument('--data-root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--budget', type=float, default=60)
    parser.add_argument('--timeout', type=float, default=180)
    parser.add_argument('--explicit', action='store_true', help='time UPDATE and COMMIT separately')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if min(args.rows, args.width, args.batch) <= 0 or args.budget <= 0 or args.timeout <= 0:
        parser.error('sizes and deadlines must be positive')
    name = 'mvdb-latency-' + uuid.uuid4().hex[:12]
    records = []
    ident = 0
    with tempfile.TemporaryDirectory(prefix='mvdb-latency-', dir=args.data_root) as work:
        data = Path(work) / 'data'
        data.mkdir(mode=0o777)
        data.chmod(0o777)
        with (Path(work) / 'stderr.log').open('w+') as errors:
            proc = subprocess.Popen([
                'docker', 'run', '--rm', '-i', '--name', name, '--network=none',
                '--cpus=2', '--memory=2g', '--memory-swap=2g', '--pids-limit=128',
                '--read-only', '--cap-drop=ALL', '--security-opt=no-new-privileges',
                '--mount', f'type=bind,src={data},dst=/data', args.image, '--stats'],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=errors,
                text=True, bufsize=1)

            def response(phase):
                started = time.monotonic()
                reported = False
                with selectors.DefaultSelector() as events:
                    events.register(proc.stdout, selectors.EVENT_READ)
                    while True:
                        if events.select(5):
                            line = proc.stdout.readline()
                            if not line:
                                raise RuntimeError(f'database exited during {phase}')
                            return json.loads(line)
                        elapsed = time.monotonic() - started
                        if elapsed >= args.budget and not reported:
                            print(f'{phase}: still waiting after {elapsed:.1f}s', flush=True)
                            reported = True
                        if elapsed >= args.timeout:
                            raise TimeoutError(phase)

            def execute(sql, phase):
                nonlocal ident
                ident += 1
                started = time.monotonic()
                proc.stdin.write(json.dumps({'id': ident, 'op': 'execute', 'sql': sql,
                                             'search_mode': 'exact'}) + '\n')
                proc.stdin.flush()
                result = response(phase)
                elapsed = time.monotonic() - started
                if result.get('id') != ident or result.get('ok') is not True:
                    raise AssertionError((phase, result))
                records.append({'phase': phase, 'seconds': elapsed})
                if phase != 'load':
                    print(f'{phase}: {elapsed:.3f}s', flush=True)
                return result

            try:
                if response('ready') != {'type': 'ready'}:
                    raise AssertionError('invalid ready response')
                execute(f'CREATE TABLE t(id BIGINT PRIMARY KEY,s VARCHAR({args.width}))', 'create')
                initial = 'x' * args.width
                changed = 'z' * args.width
                for start in range(0, args.rows, args.batch):
                    end = min(start + args.batch, args.rows)
                    result = execute('INSERT INTO t VALUES ' + ','.join(
                        f"({i},'{initial}')" for i in range(start, end)), 'load')
                    if result.get('affected_rows') != end - start:
                        raise AssertionError('incorrect inserted row count')
                    if end % 50000 == 0 or end == args.rows:
                        print(f'loaded {end}/{args.rows}', flush=True)
                if execute(f'SELECT id FROM t WHERE id={args.rows-1}', 'point_select')['rows'] != [[args.rows-1]]:
                    raise AssertionError('point lookup mismatch')
                if args.explicit:
                    execute('BEGIN', 'begin')
                result = execute(f"UPDATE t SET s='{changed}'", 'update' if args.explicit else 'autocommit_update')
                if result.get('affected_rows') != args.rows:
                    raise AssertionError('incorrect updated row count')
                if args.explicit:
                    execute('COMMIT', 'commit')
                for key in sorted({0, args.rows // 2, args.rows - 1}):
                    if execute(f'SELECT s FROM t WHERE id={key}', f'verify_{key}')['rows'] != [[changed]]:
                        raise AssertionError(f'updated row mismatch: {key}')
                shutdown_started = time.monotonic()
                proc.stdin.write(json.dumps({'id': ident+1, 'op': 'shutdown'}) + '\n')
                proc.stdin.flush()
                if response('shutdown').get('ok') is not True:
                    raise AssertionError('shutdown failed')
                records.append({'phase': 'shutdown', 'seconds': time.monotonic() - shutdown_started})
                proc.stdin.close()
                if proc.wait(timeout=60) != 0:
                    raise AssertionError('nonzero database exit')
            finally:
                subprocess.run(['docker', 'rm', '-f', name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if proc.poll() is None:
                    proc.wait(timeout=30)
                for stream in (proc.stdin, proc.stdout):
                    if stream is not None and not stream.closed:
                        stream.close()
                errors.seek(0)
                stderr = errors.read()
                print(stderr, end='', flush=True)
                if args.report:
                    args.report.write_text(json.dumps({'image': args.image, 'rows': args.rows,
                        'width': args.width, 'budget_seconds': args.budget,
                        'timings': records, 'stderr': stderr}, indent=2), encoding='utf-8')
    slow = [r for r in records if r['seconds'] > args.budget]
    if slow:
        raise AssertionError(f'response budget exceeded: {slow}')
    print('all measured requests satisfied the response budget', flush=True)


if __name__ == '__main__':
    main()
