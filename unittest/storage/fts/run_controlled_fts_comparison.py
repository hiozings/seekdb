#!/usr/bin/env python3
"""Run one equal-resource comparison with authorized temporary CPU governors.

Requires explicit user authorization. Only explicitly authorized CPU governor files are exposed
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
PATHS = {cpu: Path(f'/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor') for cpu in (6, 8, 10, 14, 16)}


def read_governors(cpus=(8, 10)):
    if not cpus or any(cpu not in PATHS for cpu in cpus):
        raise ValueError('CPU governor path is not allowlisted')
    return {cpu: PATHS[cpu].read_text().strip() for cpu in cpus}


def set_governors(image, values):
    if not values or any(cpu not in PATHS for cpu in values):
        raise ValueError('CPU governor path is not allowlisted')
    if any(value not in ('powersave', 'performance') for value in values.values()):
        raise ValueError('unexpected governor value')
    command = ['docker', 'run', '--rm', '--network', 'none', '--read-only', '--cap-drop', 'ALL']
    cpus = sorted(values)
    for cpu in cpus:
        command += ['--mount', f'type=bind,source={PATHS[cpu]},target=/governor{cpu}']
    script = 'set -e; ' + '; '.join(
        f'printf "%s" "${i}" > /governor{cpu}' for i, cpu in enumerate(cpus, 1))
    command += [image, 'sh', '-c', script, 'sh'] + [values[cpu] for cpu in cpus]
    subprocess.run(command, check=True)


def interrupted(signum, frame):
    raise KeyboardInterrupt(f'interrupted by signal {signum}')


def execute(args):
    output = args.output.resolve()
    relative = output.relative_to(REPO)
    output.mkdir(parents=True, exist_ok=False)
    cpus = tuple(int(cpu) for cpu in args.governor_cpus.split(','))
    requested = {cpu: 'performance' for cpu in cpus}
    state = {'before': read_governors(cpus), 'requested': requested,
             'user_authorized': True, 'image': args.image, 'benchmark_completed': False,
             'cpu_count': args.cpu_count, 'server_cpus': args.server_cpus, 'client_cpu': args.client_cpu}
    evidence = output / 'governors.json'
    evidence.write_text(json.dumps(state, indent=2))
    try:
        set_governors(args.image, requested)
        state['during'] = read_governors(cpus)
        evidence.write_text(json.dumps(state, indent=2))
        if any(value != 'performance' for value in state['during'].values()):
            raise RuntimeError('governor change did not take effect')
        subprocess.run(['docker', 'exec', '-e', 'GIT_CONFIG_COUNT=1',
                        '-e', 'GIT_CONFIG_KEY_0=safe.directory', '-e', 'GIT_CONFIG_VALUE_0=/work',
                        CONTAINER, 'python3', '/work/unittest/storage/fts/run_fts_comparison.py',
                        '--baseline', args.baseline,
                        '--candidate', args.candidate,
                        '--output', '/work/' + str(relative / 'runs'), '--port', '12886',
                        '--cpu-count', str(args.cpu_count), '--server-cpus', args.server_cpus,
                        '--client-cpu', args.client_cpu, '--collect-storage'], check=True)
        state['benchmark_completed'] = True
    finally:
        # Restoration must run even if setting the second CPU or the benchmark fails.
        try:
            set_governors(args.image, state['before'])
        finally:
            state['after'] = read_governors(cpus)
            state['restored'] = state['after'] == state['before']
            evidence.write_text(json.dumps(state, indent=2))
        if not state['restored']:
            raise RuntimeError('CPU governor restoration requires attention: ' + str(state))
    print('PASS: benchmark complete; all requested governors restored', flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', required=True, help='Already installed utility image digest')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--governor-cpus', default='8,10', help='Explicitly authorized CPU governor files to change and restore')
    parser.add_argument('--cpu-count', type=int, default=2)
    parser.add_argument('--server-cpus', default='8,10')
    parser.add_argument('--client-cpu', default='12')
    parser.add_argument('--baseline', default='/work/build_bench/bin/official-5bce-seekdb',
                        help='Baseline binary path inside the test container')
    parser.add_argument('--candidate', default='/work/build_bench/bin/candidate-20261101-raw',
                        help='Candidate binary path inside the test container')
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, interrupted)
    execute(args)
