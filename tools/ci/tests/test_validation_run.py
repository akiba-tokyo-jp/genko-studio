"""ログ収集を再実行にせず、一回のCTest記録を監査する。"""
import importlib.util
from pathlib import Path
import unittest
import json
import hashlib
import tempfile

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('validation_run', ROOT / 'tools/ci/validation_run.py')
assert spec is not None and spec.loader is not None
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


def log(name='test_gui_layout', totals='19 passed, 0 failed, 0 skipped', extra=''):
    return (f'Start 1: {name}\n1: PASS   : Example::initTestCase()\n{extra}\n'
            f'1: Totals: {totals}, 12ms\n100% tests passed, 0 tests failed out of 1\n').encode()


class LogTests(unittest.TestCase):
    def test_valid_single_run(self):
        self.assertEqual(runner.audit_log(log(), ['test_gui_layout'], 'windows-debug')['failures'], [])

    def test_missing_test_is_rejected(self):
        self.assertTrue(runner.audit_log(log(), ['test_gui_layout', 'test_json'], 'windows-debug')['failures'])

    def test_wrong_windows_totals_are_rejected(self):
        self.assertTrue(runner.audit_log(log(totals='18 passed, 0 failed, 0 skipped'), ['test_gui_layout'], 'windows-debug')['failures'])

    def test_raw_non_utf8_is_accepted_without_losing_evidence(self):
        self.assertFalse(runner.audit_log(log(extra='1: data') + b'\xd7\n', ['test_gui_layout'], 'windows-debug')['failures'])

    def test_utf8_slot_data_is_preserved_as_exact_bytes(self):
        raw = log(extra='1: PASS   : Example::x(日本語)')
        result = runner.audit_log(raw, ['test_gui_layout'], 'windows-debug')
        self.assertIn('Example::x(日本語)'.encode().hex(), result['passes'])

    def test_windows_file_log_is_audited_without_rerunning(self):
        raw = b'    Start 1: test_gui_layout\n1/1 Test #1: test_gui_layout ........ Passed\n100% tests passed, 0 tests failed out of 1\n'
        qfile = b'PASS   : Example::x()\nTotals: 19 passed, 0 failed, 0 skipped, 0 blacklisted, 1ms\n'
        result = runner.audit_log(raw, ['test_gui_layout'], 'windows-debug', {'test_gui_layout': qfile})
        self.assertEqual(result['failures'], [])
        self.assertEqual(result['totals']['test_gui_layout'][0], 19)

    def test_windows_missing_file_cannot_reuse_old_stdout(self):
        result = runner.audit_log(log(), ['test_gui_layout'], 'windows-debug', {})
        self.assertTrue(result['failures'])

    def test_duplicate_totals_are_rejected(self):
        self.assertTrue(runner.audit_log(log() + b'1: Totals: 19 passed, 0 failed, 0 skipped\n', ['test_gui_layout'], 'windows-debug')['failures'])

    def test_missing_totals_are_rejected(self):
        self.assertTrue(runner.audit_log(b'Start 1: test_json\n100% tests passed, 0 tests failed out of 1\n', ['test_json'], 'linux-debug')['failures'])

    def test_debug_skip_is_failure(self):
        data = log('test_json', '1 passed, 0 failed, 1 skipped', '1: SKIP   : Example::x() unavailable')
        self.assertTrue(runner.audit_log(data, ['test_json'], 'linux-debug')['failures'])

    def test_release_unknown_skip_is_failure(self):
        data = log('test_json', '1 passed, 0 failed, 1 skipped', '1: SKIP   : Example::x() missing Python')
        self.assertTrue(runner.audit_log(data, ['test_json'], 'linux-release')['failures'])

    def test_known_release_skip_requires_pair_later(self):
        data = log('test_app_e2e', '0 passed, 0 failed, 1 skipped', '1: SKIP   : TestAppE2e::initTestCase() test scripts are only in builds with fault injection (not a release configuration)')
        d = runner.audit_log(data, ['test_app_e2e'], 'windows-release')
        self.assertFalse(d['failures'])
        self.assertEqual(len(d['skips_to_pair']), 1)

    def test_sanitizer_output_is_failure_even_with_ctest_green(self):
        self.assertTrue(runner.audit_log(log(extra='runtime error: invalid cast'), ['test_gui_layout'], 'windows-debug')['failures'])


class ConfigurationTests(unittest.TestCase):
    def cache(self, preset='linux-asan'):
        release = preset.endswith('release')
        return '\n'.join(['GENKO_WERROR:BOOL=ON', 'GENKO_FAULT_INJECTION:BOOL=' + ('OFF' if release else 'ON'), 'CMAKE_BUILD_TYPE:STRING=' + ('Release' if release else 'Debug'), 'GENKO_SANITIZE:STRING=' + ('address,undefined' if preset == 'linux-asan' else '')])

    def test_asan_requires_real_sanitizer_build(self):
        self.assertTrue(runner.configuration_errors(self.cache('linux-debug'), 'linux-asan'))

    def test_expected_presets_match(self):
        for preset in ('linux-debug', 'linux-release', 'linux-asan', 'windows-debug', 'windows-release'):
            self.assertEqual(runner.configuration_errors(self.cache(preset), preset), [])

    def test_warning_and_fault_settings_cannot_be_disabled(self):
        self.assertTrue(runner.configuration_errors(self.cache().replace('GENKO_WERROR:BOOL=ON', 'GENKO_WERROR:BOOL=OFF'), 'linux-asan'))
        self.assertTrue(runner.configuration_errors(self.cache().replace('GENKO_FAULT_INJECTION:BOOL=ON', 'GENKO_FAULT_INJECTION:BOOL=OFF'), 'linux-asan'))


class PairTests(unittest.TestCase):
    def evidence(self, folder):
        plan = {'phase': 'development', 'matrix': {'include': [{'preset': 'linux-debug', 'tests': ['test_gui_layout'], 'suite': 'related', 'excluded_contracts': []}]}}
        out = Path(folder) / 'linux-debug'; out.mkdir()
        raw = log()
        (out / 'ctest-output.log').write_bytes(raw)
        (out / 'build.log').write_bytes(b'unit fixture build log')
        (out / 'registered.json').write_text(json.dumps({'tests': [{'name': 'test_gui_layout'}]}))
        state = {'preset': 'linux-debug', 'phase': 'development', 'suite': 'related', 'selected': ['test_gui_layout'], 'excluded_contracts': [], 'status': 'finished', 'passed': True, 'failures': [], 'ctest_exit': 0, 'cli23': 'not_run', 'configuration': ConfigurationTests().cache('linux-debug'), 'plan_hash': runner.plan_hash(plan), 'source_before': {'native/example.cpp': 'a' * 64}, 'source_after': {'native/example.cpp': 'a' * 64}, 'binary_before': {'test_gui_layout': 'b' * 64}, 'binary_after': {'test_gui_layout': 'b' * 64}, 'audit': runner.audit_log(raw, ['test_gui_layout'], 'linux-debug')}
        (out / 'result.json').write_text(json.dumps(state))
        return plan, out, state

    def test_complete_evidence_passes(self):
        with tempfile.TemporaryDirectory() as folder:
            plan, _, _ = self.evidence(folder)
            self.assertTrue(runner.pair_results(folder, plan)['passed'])

    def test_partial_artifact_missing_log_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            plan, out, _ = self.evidence(folder)
            (out / 'ctest-output.log').unlink()
            self.assertFalse(runner.pair_results(folder, plan)['passed'])

    def test_changed_raw_log_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            plan, out, _ = self.evidence(folder)
            (out / 'ctest-output.log').write_bytes(b'changed')
            self.assertFalse(runner.pair_results(folder, plan)['passed'])

    def test_related_cannot_become_full_by_renaming_plan(self):
        with tempfile.TemporaryDirectory() as folder:
            plan, _, _ = self.evidence(folder)
            plan['phase'] = 'milestone'; plan['matrix']['include'][0]['suite'] = 'full'
            self.assertFalse(runner.pair_results(folder, plan)['passed'])

    def test_pins_cannot_be_missing(self):
        with tempfile.TemporaryDirectory() as folder:
            plan, out, state = self.evidence(folder)
            state.pop('binary_before'); state.pop('source_before')
            (out / 'result.json').write_text(json.dumps(state))
            self.assertFalse(runner.pair_results(folder, plan)['passed'])

    def test_duplicate_result_fails(self):
        with tempfile.TemporaryDirectory() as folder:
            plan, out, state = self.evidence(folder)
            other = Path(folder) / 'duplicate'; other.mkdir()
            (other / 'result.json').write_text(json.dumps(state))
            self.assertFalse(runner.pair_results(folder, plan)['passed'])


if __name__ == '__main__':
    unittest.main()
