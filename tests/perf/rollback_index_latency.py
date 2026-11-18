#!/usr/bin/env python3
"""Exercise a complete correctness case under the public transport deadlines."""
import argparse
import json
from pathlib import Path
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'protocol_check'))
from _transport import Session


def run(session, cycles):
    def execute(sql, mode='exact'):
        response = session.request(sql, mode)
        if response.get('ok') is not True:
            raise AssertionError(response)
        return response

    def measure(name, operation):
        start = time.monotonic()
        try:
            return operation()
        finally:
            print(f'{name}: {time.monotonic() - start:.3f}s', flush=True)

    rows, dim = 5000, 1024
    measure('initial ready', session.start)
    execute(f'CREATE TABLE t(id BIGINT PRIMARY KEY, g BIGINT,v VECTOR({dim}))')
    coordinates = ['0'] * dim
    for start in range(0, rows, 100):
        batch = []
        for pk in range(start, start + 100):
            coordinates[pk % dim] = '1'
            batch.append(f"({pk},0,'[{','.join(coordinates)}]')")
            coordinates[pk % dim] = '0'
        execute('INSERT INTO t VALUES ' + ','.join(batch))

    measure('create vector index', lambda: execute('CREATE VECTOR INDEX ix ON t(v)'))
    measure('bulk scalar update', lambda: execute('UPDATE t SET g=1'))
    query = f"SELECT id,g FROM t ORDER BY L2_DISTANCE(v,'[{','.join(coordinates)}]') LIMIT 10"
    # All vectors have exactly unit length. Primary key breaks the tie.
    expected = [[i, 1] for i in range(10)]

    def check_topk():
        actual = execute(query, 'ann')['rows']
        if actual != expected:
            raise AssertionError((expected, actual))

    measure('ann', check_topk)
    for cycle in range(cycles):
        execute('BEGIN')
        execute('UPDATE t SET g=0 WHERE id=0')
        execute('ROLLBACK')
        measure(f'ann after rollback {cycle}', check_topk)
    measure('shutdown', session.shutdown)
    measure('restart ready', session.start)
    measure('ann after restart', check_topk)
    session.shutdown()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument('--bin', type=Path)
    target.add_argument('--image')
    parser.add_argument('--cycles', type=int, default=55)
    args = parser.parse_args()
    config = json.loads((ROOT / 'protocol_check/config.json').read_text())
    with tempfile.TemporaryDirectory() as directory:
        session = Session(config, Path(directory) / 'stderr.log', image=args.image,
                          command=[str(args.bin.resolve()), '--data-dir', '{data_dir}'] if args.bin else None)
        start = time.monotonic()
        try:
            run(session, args.cycles)
        finally:
            session.close()
            print(f'total: {time.monotonic() - start:.3f}s', flush=True)


if __name__ == '__main__':
    main()
