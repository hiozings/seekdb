#!/usr/bin/env python3
"""Governor restoration tests; no Docker process or host setting is touched."""
import json
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import run_controlled_fts_comparison as controlled


class GovernorRestorationTest(unittest.TestCase):
    def test_restores_after_benchmark_or_partial_setting_failure(self):
        for partial_failure in (False, True):
            with self.subTest(partial_failure=partial_failure), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                before = {8: 'powersave', 10: 'performance', 14: 'powersave', 16: 'powersave'}
                state = dict(before)
                changes = []

                def read(cpus=(8, 10)):
                    return {cpu: state[cpu] for cpu in cpus}

                def change(image, values):
                    changes.append(dict(values))
                    if partial_failure and len(changes) == 1:
                        state[8] = values[8]
                        raise RuntimeError('failed after changing one governor')
                    state.update(values)

                args = SimpleNamespace(output=root / 'result', image='unused',
                                       governor_cpus='8,10,14,16', cpu_count=4,
                                       server_cpus='8,10,14,16', client_cpu='12',
                                       baseline='/work/base', candidate='/work/candidate')
                with patch.object(controlled, 'REPO', root), \
                        patch.object(controlled, 'read_governors', side_effect=read), \
                        patch.object(controlled, 'set_governors', side_effect=change), \
                        patch.object(controlled.subprocess, 'run',
                                     side_effect=subprocess.CalledProcessError(1, 'benchmark')):
                    with self.assertRaises((RuntimeError, subprocess.CalledProcessError)):
                        controlled.execute(args)
                self.assertEqual(before, state)
                self.assertEqual(before, changes[-1])
                evidence = json.loads((args.output / 'governors.json').read_text())
                self.assertTrue(evidence['restored'])
                self.assertFalse(evidence['benchmark_completed'])

    def test_rejects_unapproved_paths_and_values_before_docker(self):
        with patch.object(controlled.subprocess, 'run') as run:
            for values in ({0: 'performance'}, {8: 'other'}, {}):
                with self.subTest(values=values), self.assertRaises(ValueError):
                    controlled.set_governors('unused', values)
            run.assert_not_called()


if __name__ == '__main__':
    unittest.main()
