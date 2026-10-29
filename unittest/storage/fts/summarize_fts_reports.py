#!/usr/bin/env python3
"""Summarize all complete paired runs; CI score and local comparison stay separate."""
import argparse
import importlib.util
import json
from pathlib import Path
from statistics import mean, stdev

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('official_fts_score', REPO / 'tools/benchmark/fts_large_bench_score.py')
score = importlib.util.module_from_spec(spec)
spec.loader.exec_module(score)


def summarize(directories, baseline_label="baseline"):
    config = score.load_baseline(score.DEFAULT_BASELINE)
    groups = {name: sorted(p for directory in directories for p in directory.glob('*-' + (baseline_label if name == 'baseline' else name) + '.report'))
              for name in ('baseline', 'candidate')}
    if not groups['baseline'] or len(groups['baseline']) != len(groups['candidate']):
        raise ValueError('equal nonzero numbers of baseline and candidate reports required')
    reports = {}
    for name, paths in groups.items():
        reports[name] = [score.load_report(path) for path in paths]
        for report in reports[name]:
            score.check_config(report, config, False)
            score.calculate_score(report, config) # Reject missing scoring metrics.
    means = {name: {metric: mean(report[metric] for report in values) for metric in values[0]}
             for name, values in reports.items()}
    categories = {}
    for name, metrics in config['score_categories'].items():
        values = []
        for metric in metrics:
            before = means['baseline'][metric]
            after = means['candidate'][metric]
            values.append({'metric': metric, 'before': before, 'after': after,
                           'improvement': (before - after) / before,
                           'before_runs': [r[metric] for r in reports['baseline']],
                           'after_runs': [r[metric] for r in reports['candidate']]})
        categories[name] = {'improvement': mean(v['improvement'] for v in values), 'metrics': values}
    return {'baseline_label': baseline_label, 'runs_per_version': len(groups['baseline']), 'reports': {n: [str(p) for p in ps] for n, ps in groups.items()},
            'categories': categories, 'local_mean_improvement': mean(c['improvement'] for c in categories.values()),
            'ci_reference_only': score.calculate_score(means['candidate'], config),
            'note': 'Local measurements are not official CI scores. No per-metric negative clipping. All runs included.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directories', type=Path, nargs='+')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--baseline-label', choices=['baseline', 'previous'], default='baseline')
    args = parser.parse_args()
    result = summarize(args.directories, args.baseline_label)
    args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
    print('Local weighted time reduction: %.3f%% (%d runs/version)' %
          (100 * result['local_mean_improvement'], result['runs_per_version']))
