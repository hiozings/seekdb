#!/usr/bin/env python3
"""Run one equal-resource comparison with authorized temporary CPU governors.

Requires explicit user authorization. Only CPU 8/10 governor files are exposed
to a networkless, capability-free utility container. Original values are saved
before mutation and restored in finally. Does not alter official benchmarks.
"""
import argparse
import json
from pathlib import Path
import signal
import subprocess

REPO = Path(__file__).resolve().parents[3]
CONTAINER = 'seekdb-official-tests-20261028'
PATHS = {cpu: Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor') for cpu in (8, 10)}


def read_governors():
    return {cpu: path.read_text().strip() for cpu, path in PATHS.items()}


def set_governors(image, values):
    if any(value not in ('powersave', 'performance') for value in values.values()):
        raise ValueError('unexpected governor value')
    command = ['docker', 'run', '--rm', '--network', 'none', '--read-only', '--cap-drop', 'ALL']
    for cpu, path in PATHS.items():
        command += ['--mount', f'type=bind,source={path},target=/governor{cpu}']
    command += [image, 'sh', '-c',
                'set -e; printf "%s" "$1" > /governor8; printf "%s" "$2" > /governor10',
                'sh', values[8], values[10]]
    subprocess.run(command, check=True)


def interrupted(signum, frame):
    raise KeyboardInterrupt(f'interrupted by signal {signum}')


def execute(args):
    output = args.output.resolve()
    relative = output.relative_to(REPO)
    output.mkdir(parents=True, exist_ok=False)
    state = {'before': read_governors(), 'requested': {8: 'performance', 10: 'performance'},
             'user_authorized': True, 'image': args.image, 'benchmark_completed': False}
    evidence = output / 'governors.json'
    evidence.write_text(json.dumps(state, indent=2))
    try:
        set_governors(args.image, {8: 'performance', 10: 'performance'})
        state['during'] = read_governors()
        evidence.write_text(json.dumps(state, indent=2))
        if any(value != 'performance' for value in state['during'].values()):
            raise RuntimeError('governor change did not take effect')
        subprocess.run(['docker', 'exec', '-e', 'GIT_CONFIG_COUNT=1',
                        '-e', 'GIT_CONFIG_KEY_0=safe.directory', '-e', 'GIT_CONFIG_VALUE_0=/work',
                        CONTAINER, 'python3', '/work/unittest/storage/fts/run_fts_comparison.py',
                        '--baseline', '/work/build_bench/bin/official-5bce-seekdb',
                        '--candidate', '/work/build_bench/bin/candidate-20261101-raw',
                        '--output', '/work/' + str(relative / 'runs'), '--port', '12886',
                        '--server-cpus', '8,10', '--client-cpu', '12', '--collect-storage'], check=True)
        state['benchmark_completed'] = True
    finally:
        # Restoration must run even if setting the second CPU or the benchmark fails.
        try:
            set_governors(args.image, state['before'])
        finally:
            state['after'] = read_governors()
            state['restored'] = state['after'] == state['before']
            evidence.write_text(json.dumps(state, indent=2))
        if not state['restored']:
            raise RuntimeError('CPU governor restoration requires attention: ' + str(state))
    print('PASS: benchmark complete; both governors restored', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, help='Already installed utility image digest')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, interrupted)
    execute(args)
