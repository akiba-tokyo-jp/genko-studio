"""前のCIが失敗していた変更を、直前commit差分だけで落とさない。"""
import importlib.util
from pathlib import Path
import unittest
from unittest import mock
import contextlib
import io
import json
import tempfile

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('validation_policy', ROOT / 'tools/ci/validation_policy.py')
assert spec and spec.loader
policy = importlib.util.module_from_spec(spec); spec.loader.exec_module(policy)


class BaselineTests(unittest.TestCase):
    def row(self, sha='a' * 40, branch='native/integration', conclusion='success'):
        return {'head_sha': sha, 'head_branch': branch, 'event': 'push', 'status': 'completed', 'conclusion': conclusion, 'html_url': 'https://github.com/example/runs/1'}

    def test_last_pass_not_last_failed_commit(self):
        rows = [self.row('b' * 40, conclusion='failure'), self.row()]
        self.assertEqual(policy.choose_baseline(rows, 'native/integration', lambda sha: True)['head_sha'], 'a' * 40)

    def test_other_branch_and_unrelated_history_are_not_reused(self):
        rows = [self.row(branch='native/feature'), self.row('c' * 40), self.row()]
        self.assertEqual(policy.choose_baseline(rows, 'native/integration', lambda sha: sha != 'c' * 40)['head_sha'], 'a' * 40)

    def test_no_verified_baseline_requires_full(self):
        self.assertIsNone(policy.choose_baseline([self.row(conclusion='cancelled')], 'native/integration', lambda sha: True))

    def test_malformed_sha_is_not_repaired(self):
        self.assertIsNone(policy.choose_baseline([self.row('not-a-sha')], 'native/integration', lambda sha: True))


    def run_main(self, answer=None, error=None):
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / 'plan.json'
            args = ['validation_policy.py', '--base', 'b' * 40, '--phase', 'integration', '--ci-baseline-branch', 'native/integration', '--repository', 'example/repo', '--output', str(output)]
            with mock.patch('sys.argv', args), mock.patch.object(policy, 'last_successful_push', return_value=answer, side_effect=error) as api, mock.patch.object(policy, 'changed_paths', return_value=['native/src/core/pynum.cpp']) as diff, contextlib.redirect_stdout(io.StringIO()):
                policy.main()
            return json.loads(output.read_text()), api.call_count, diff.call_args, diff.call_count

    def test_main_uses_last_pass_sha_for_whole_gap(self):
        plan, calls, diff, _ = self.run_main(self.row())
        self.assertEqual(calls, 1)
        self.assertEqual(diff.args[1], 'a' * 40)
        self.assertEqual(plan['base'], 'a' * 40)

    def test_main_api_failure_escalates_full_without_shortening_diff(self):
        plan, calls, _, diffs = self.run_main(error=OSError('unit fixture unavailable'))
        self.assertEqual((calls, diffs), (1, 0))
        self.assertIsNone(plan['base'])
        self.assertEqual(plan['tier'], 'full')
        self.assertTrue(all(r['suite'] == 'full' for r in plan['matrix']['include']))

    def test_main_no_verified_baseline_escalates_full(self):
        plan, calls, _, diffs = self.run_main()
        self.assertEqual((calls, diffs), (1, 0))
        self.assertEqual(plan['tier'], 'full')


if __name__ == '__main__':
    unittest.main()
