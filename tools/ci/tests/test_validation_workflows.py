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

    def test_milestone_pr_works_before_workflow_is_on_default_branch(self):
        text = (ROOT / '.github/workflows/native.yml').read_text()
        self.assertIn("startsWith(github.head_ref, 'native/milestone/')", text)
        self.assertIn('MILESTONE_PR:', text)
        self.assertIn('PHASE=milestone', text)

    def test_pr_evidence_is_exact_head_not_a_different_merge_tree(self):
        text = (ROOT / '.github/workflows/native.yml').read_text()
        self.assertEqual(text.count('ref: ${{ github.event.pull_request.head.sha || github.sha }}'), text.count('uses: actions/checkout@v4'))
        self.assertIn('WORKFLOW_SHA: ${{ github.workflow_sha }}', text)
        self.assertIn('git diff --quiet "$WORKFLOW_SHA" HEAD -- .github/workflows/native.yml', text)

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
