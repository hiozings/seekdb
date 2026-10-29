"""Check local category weighting and missing/config-invalid report rejection."""
import json
from pathlib import Path
import tempfile
import unittest
import summarize_fts_reports as summary


class ReportSummaryTests(unittest.TestCase):
    def test_equal_category_weight_and_negative_items(self):
        config = summary.score.load_baseline(summary.score.DEFAULT_BASELINE)
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            metrics = dict(config['benchmark_config'])
            for names in config['score_categories'].values():
                metrics.update({name: 100 for name in names})
            (directory / '1-baseline.report').write_text('\n'.join(f'{k}: {v}' for k, v in metrics.items()))
            for names in config['score_categories'].values():
                for name in names:
                    metrics[name] = 50
            metrics['build_ik_all_sec'] = 200 # A regression must remain negative.
            (directory / '2-candidate.report').write_text('\n'.join(f'{k}: {v}' for k, v in metrics.items()))
            result = summary.summarize([directory])
            self.assertAlmostEqual(0, result['categories']['build']['improvement'])
            self.assertAlmostEqual(1 / 3, result['local_mean_improvement'])
            metrics.pop('query_cn_avg_ms')
            (directory / '2-candidate.report').write_text('\n'.join(f'{k}: {v}' for k, v in metrics.items()))
            with self.assertRaises(SystemExit):
                summary.summarize([directory])
            metrics['query_cn_avg_ms'] = 50
            metrics['skip_load'] = 1
            (directory / '2-candidate.report').write_text('\n'.join(f'{k}: {v}' for k, v in metrics.items()))
            with self.assertRaises(SystemExit):
                summary.summarize([directory])

    def test_unpaired_reports_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(ValueError):
                summary.summarize([Path(folder)])


if __name__ == '__main__':
    unittest.main()
