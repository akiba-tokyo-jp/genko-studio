"""CI入口の無条件除外・異なる検証の取消しの再発を防ぐ。"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class WorkflowTests(unittest.TestCase):
    def test_native_unknown_paths_reach_the_planner(self):
        raw = (ROOT / '.github/workflows/native.yml').read_text()
        self.assertNotIn('paths:', raw.split('jobs:')[0])
        self.assertIn('fetch-depth: 0', raw)

    def test_fork_main_pr_is_not_skipped_by_branch_name(self):
        for name in ('ci.yml', 'native.yml'):
            raw = (ROOT / '.github/workflows' / name).read_text()
            self.assertNotIn('github.head_ref !=', raw)

    def test_milestone_does_not_share_cancel_group_with_push(self):
        raw = (ROOT / '.github/workflows/native.yml').read_text()
        self.assertIn('cancel-in-progress: false', raw)
        self.assertIn("${{ inputs.phase || 'integration' }}", raw)
        self.assertIn('${{ github.event_name }}', raw)

    def test_success_log_does_not_rerun_qtest_executables(self):
        raw = (ROOT / '.github/workflows/native.yml').read_text()
        self.assertNotIn('& $exe', raw)
        self.assertNotIn('Retry failures', raw)
        self.assertIn('validation_run.py', raw)

    def test_python_workflow_has_change_filters(self):
        raw = (ROOT / '.github/workflows/ci.yml').read_text()
        self.assertEqual(raw.count('paths:'), 2)
        self.assertNotIn('"native/**"', raw)
        self.assertIn('"src/**"', raw)


if __name__ == '__main__':
    unittest.main()
