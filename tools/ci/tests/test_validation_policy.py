"""検証頻度・選択・安全側への拡大を確認する回帰試験。"""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('validation_policy', ROOT / 'tools/ci/validation_policy.py')
assert spec is not None and spec.loader is not None
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class PolicyTests(unittest.TestCase):
    def plan(self, paths, phase='development'):
        return policy.make_plan(paths, phase, ROOT)

    def test_docs_do_not_start_native_builds(self):
        p = self.plan(['docs/cpp-migration/PLAN.md'], 'integration')
        self.assertEqual(p['matrix']['include'], [])
        self.assertEqual(p['tier'], 'documents')

    def test_milestone_runs_all_three_linux_configurations(self):
        p = self.plan(['docs/cpp-migration/PLAN.md'], 'milestone')
        m = {r['preset']: r for r in p['matrix']['include']}
        for name in ('linux-debug', 'linux-release', 'linux-asan'):
            self.assertEqual(m[name]['suite'], 'full')
        self.assertIn('windows-debug', m)
        self.assertIn('windows-release', m)

    def test_integration_has_one_full_contract_run(self):
        p = self.plan(['native/src/render/raster_ops.cpp'], 'integration')
        m = {r['preset']: r for r in p['matrix']['include']}
        self.assertEqual(m['linux-release']['suite'], 'full')
        self.assertEqual(m['linux-asan']['suite'], 'related')
        self.assertNotIn('linux-debug', m)
        self.assertNotIn('test_contract_3d_render', m['linux-asan']['tests'])
        self.assertIn('test_contract_raster_ops', m['linux-asan']['tests'])

    def test_core_change_escalates_full_sanitizers(self):
        p = self.plan(['native/src/core/pynum.cpp'], 'integration')
        self.assertTrue(all(r['suite'] == 'full' for r in p['matrix']['include']))

    def test_unknown_native_file_escalates(self):
        p = self.plan(['native/src/future/new.cpp'])
        self.assertEqual(p['tier'], 'full')
        self.assertTrue(all(r['suite'] == 'full' for r in p['matrix']['include']))

    def test_missing_diff_escalates_not_empty(self):
        self.assertEqual(self.plan([])['tier'], 'full')

    def test_mixed_docs_and_source_is_not_documents(self):
        self.assertNotEqual(self.plan(['docs/x.md', 'native/src/app/canvas.cpp'])['tier'], 'documents')

    def test_ci_mixed_with_contract_never_drops_changed_contract(self):
        paths = ['tools/ci/validation_policy.py', 'native/tests/contract/test_contract_raster_ops.cpp']
        for phase in ('development', 'integration'):
            rows = {r['preset']: r for r in policy.make_plan(paths, phase, ROOT)['matrix']['include']}
            for preset, row in rows.items():
                if preset.startswith('linux'):
                    self.assertIn('test_contract_raster_ops', row['tests'])
            self.assertTrue({'linux-release', 'linux-asan', 'windows-debug', 'windows-release'} <= set(rows))
            for row in rows.values():
                self.assertTrue(policy.inventory(ROOT)['normal'] <= set(row['tests']))

    def test_unknown_workflow_and_non_native_files_escalate(self):
        for path in ('.github/future-validation.json', 'tools/future-validation.py'):
            plan = policy.make_plan([path], 'development', ROOT)
            self.assertEqual(plan['tier'], 'full')

    def test_policy_change_runs_real_ci_but_not_all_contracts(self):
        p = self.plan(['.github/workflows/native.yml', 'tools/ci/validation_policy.py'])
        self.assertEqual(len(p['matrix']['include']), 4)
        for r in p['matrix']['include']:
            self.assertIn('test_gui_layout', r['tests'])
            self.assertNotIn('test_contract_3d_render', r['tests'])

    def test_changed_test_is_never_omitted(self):
        p = self.plan(['native/tests/unit/test_strokes.cpp'])
        self.assertTrue(all('test_strokes' in r['tests'] for r in p['matrix']['include']))

    def test_shared_test_support_is_full(self):
        self.assertEqual(self.plan(['native/tests/support/testsupport.cpp'])['tier'], 'full')

    def test_perf_tests_remain_registered_but_separate(self):
        inventory = policy.inventory(ROOT)
        self.assertIn('test_perf_app', inventory['perf'])
        p = self.plan([], 'milestone')
        self.assertFalse(any('test_perf_app' in r['tests'] for r in p['matrix']['include']))
        self.assertIn('performance_separate', p)

    def test_windows_contract_exclusion_is_explicit(self):
        p = self.plan([], 'milestone')
        for r in p['matrix']['include']:
            if r['preset'].startswith('windows'):
                self.assertTrue(r['excluded_contracts'])
                self.assertFalse(any(n.startswith('test_contract') for n in r['tests']))

    def test_invalid_phase_is_rejected(self):
        with self.assertRaises(ValueError):
            self.plan(['native/src/app/canvas.cpp'], 'skip')

    def test_no_duplicate_targets(self):
        p = self.plan(['native/src/render/raster_ops.cpp', 'native/src/app/canvas.cpp'])
        for r in p['matrix']['include']:
            self.assertEqual(r['tests'], sorted(set(r['tests'])))


if __name__ == '__main__':
    unittest.main()
