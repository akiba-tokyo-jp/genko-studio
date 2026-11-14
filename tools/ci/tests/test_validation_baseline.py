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
    def row(self, sha='a' * 40, branch='native/integration', conclusion='success', **changes):
        row = {'head_sha': sha, 'head_branch': branch, 'event': 'push', 'status': 'completed', 'conclusion': conclusion, 'html_url': 'https://github.com/example/runs/1'}
        row.update(changes)
        return row

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


    def test_a_head_that_is_not_checked_out_is_not_classified_from_this_tree(self):
        # (the closure reads the sources of the tree it stands in: for another commit's diff, what the closure would decide
        # goes to everything, as it did before the closure; the rules by names need no tree)
        def planned(head, path):
            with tempfile.TemporaryDirectory() as folder:
                output = Path(folder) / 'plan.json'
                args = ['validation_policy.py', '--base', 'b' * 40, '--head', head, '--phase', 'integration', '--output', str(output)]
                with mock.patch('sys.argv', args), mock.patch.object(policy, 'changed_paths', return_value=[path]), contextlib.redirect_stdout(io.StringIO()):
                    policy.main()
                return json.loads(output.read_text())
        here = planned('HEAD', 'native/src/service/letter.cpp')
        self.assertEqual(here['tier'], 'integration')
        self.assertEqual({r['preset']: r['suite'] for r in here['matrix']['include']}['linux-asan'], 'related')
        other = planned('HEAD~1', 'native/src/service/letter.cpp')
        self.assertEqual(other['tier'], 'full')
        self.assertTrue(all(r['suite'] == 'full' for r in other['matrix']['include']))
        self.assertIn('--head が checkout している版と違う（閉包は今の木を読む）: 全体へ拡大', other['reasons'])
        for path in ('native/src/app/canvas.cpp', 'native/tests/unit/test_strokes.cpp', 'docs/cpp-migration/PLAN.md'):
            self.assertEqual(planned('HEAD~1', path)['matrix'], planned('HEAD', path)['matrix'], path)

    def test_a_stage_exits_pass_is_neither_a_base_nor_everything_passed(self):
        # (review P2-3, changed 2026-11-14: a passed PR of native/milestone/* used to be taken as "every test passed in
        # every configuration" — as the diff's base, and with the same tree as a plan with nothing to build. A stage exit
        # now runs ASan and Windows on what its own diff reaches, without Debug: its pass says nothing of the tests it did
        # not select. What ran everything is a run of the final phase, which the list of runs does not tell: no reuse.)
        self.assertFalse(hasattr(policy, 'choose_milestone_baseline'))
        push = self.row('c' * 40)
        milestone = self.row('d' * 40, event='pull_request', head_branch='native/milestone/example')
        asked = []

        def fetch(repository, event, branch):
            asked.append((event, branch))
            return [milestone, push] if event == 'push' else [milestone]
        with mock.patch.object(policy, 'fetch_runs', fetch), mock.patch.object(policy.subprocess, 'check_output', return_value=b'e' * 40), \
                mock.patch.object(policy.subprocess, 'run', return_value=mock.Mock(returncode=0)):
            chosen = policy.last_successful_push(ROOT, 'example/repo', 'native/integration', 'HEAD')
        self.assertEqual(chosen['head_sha'], 'c' * 40)
        self.assertEqual(asked, [('push', 'native/integration')])   # (the pull requests' runs are not even read)

    def test_the_same_tree_as_the_last_pass_is_planned_and_built(self):
        # (was: a plan of tier reused-full-milestone with nothing in it, schema 1)
        row = self.row(event='pull_request', head_branch='native/milestone/example', _verified_full_milestone=True)
        with tempfile.TemporaryDirectory() as folder:
            output = Path(folder) / 'plan.json'
            args = ['validation_policy.py', '--base', 'b' * 40, '--phase', 'integration', '--ci-baseline-branch', 'native/integration', '--repository', 'example/repo', '--output', str(output)]
            with mock.patch('sys.argv', args), mock.patch.object(policy, 'last_successful_push', return_value=row), mock.patch.object(policy, 'changed_paths', return_value=[]), contextlib.redirect_stdout(io.StringIO()):
                policy.main()
            plan = json.loads(output.read_text())
            self.assertEqual((plan['tier'], plan['schema'], plan['has_native']), ('full', 2, True))
            self.assertTrue(plan['matrix']['include'])
            self.assertTrue(all(r['suite'] == 'full' for r in plan['matrix']['include']))
            self.assertIn('差分なし・基点不明: 全体へ拡大', plan['reasons'])


if __name__ == '__main__':
    unittest.main()
