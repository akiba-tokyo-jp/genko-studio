"""ログ収集を再実行にせず、一回のCTest記録を監査する。"""
import importlib.util
from pathlib import Path
import unittest
import json
import argparse
import contextlib
import hashlib
import io
import tempfile
from unittest import mock

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

    def test_tests_run_side_by_side_are_read_one_by_one(self):
        # (ctest --parallel: the lines of two tests interleave, each with its own number)
        raw = (b'Start 1: test_json\nStart 2: test_gui_layout\n2: PASS   : Layout::initTestCase()\n1: PASS   : Json::initTestCase()\n'
               b'2: Totals: 19 passed, 0 failed, 0 skipped, 9ms\n1: Totals: 5 passed, 0 failed, 0 skipped, 3ms\n'
               b'100% tests passed, 0 tests failed out of 2\n')
        result = runner.audit_log(raw, ['test_gui_layout', 'test_json'], 'linux-release')
        self.assertEqual(result['failures'], [])
        bad = raw.replace(b'1: Totals: 5 passed, 0 failed', b'1: Totals: 4 passed, 1 failed')
        self.assertIn('test_json: QtTestが成功していない', runner.audit_log(bad, ['test_gui_layout', 'test_json'], 'linux-release')['failures'])

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

    def test_real_cache_comments_do_not_become_part_of_keys(self):
        cache = '# CMake cache\n\n//Chosen config\nCMAKE_BUILD_TYPE:STRING=Debug\n\n//Enable warnings\nGENKO_WERROR:BOOL=ON\n\n//Fault injection\nGENKO_FAULT_INJECTION:BOOL=ON\n\n//Sanitizers\nGENKO_SANITIZE:STRING=address,undefined\n'
        self.assertEqual(runner.configuration_errors(cache, 'linux-asan'), [])
        self.assertEqual(runner.configuration_errors(cache.replace('\n', '\r\n'), 'linux-asan'), [])
        self.assertTrue(runner.configuration_errors(cache.replace('GENKO_WERROR:BOOL=ON', 'GENKO_WERROR:BOOL=OFF'), 'linux-asan'))

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


NAMES = ['test_fx', 'test_fx_d2', 'test_json']


def part(name, verdict='pass', names=NAMES, extra=''):
    """One test's share of a ctest -V log: its start, its own numbered lines, and (unless it was cut) its result line."""
    number = names.index(name) + 1
    text = f'    Start {number}: {name}\n{number}: PASS   : Example::{name}()\n{extra}'
    if verdict == 'cut':
        return text.encode()
    failed = 1 if verdict == 'fail' else 0
    if failed:
        text += f'{number}: FAIL!  : Example::broken() compared values differ\n'
    text += f'{number}: Totals: 3 passed, {failed} failed, 0 skipped, 0 blacklisted, 12ms\n'
    text += f'{number}/{len(names)} Test #{number}: {name} ' + '.' * 20 + ('***Failed    0.10 sec\n' if failed else '   Passed    0.10 sec\n')
    return text.encode()


class AttemptTests(unittest.TestCase):
    def test_a_cut_run_and_the_rest_pass_together(self):
        first = part('test_fx') + part('test_fx_d2', 'cut')
        second = part('test_fx_d2') + part('test_json') + b'100% tests passed, 0 tests failed out of 2\n'
        result = runner.audit_attempts([first, second], NAMES, 'linux-asan')
        self.assertEqual(result['failures'], [])
        self.assertEqual(result['accepted_in'], {'test_fx': 1, 'test_fx_d2': 2, 'test_json': 2})
        self.assertEqual(result['rerun_after_failure'], {})
        self.assertEqual(sorted(result['totals']), NAMES)
        self.assertEqual(len(result['passes']), 3)
        self.assertEqual(result['attempt_log_sha256'], [hashlib.sha256(first).hexdigest(), hashlib.sha256(second).hexdigest()])

    def test_the_same_slots_as_one_whole_run(self):
        # (carried over or not, the release-with-asan pairing reads the same PASS rows)
        whole = b''.join(part(n) for n in NAMES) + b'100% tests passed, 0 tests failed out of 3\n'
        split = runner.audit_attempts([part('test_fx') + part('test_fx_d2', 'cut'), part('test_fx_d2') + part('test_json')], NAMES, 'linux-asan')
        once = runner.audit_log(whole, NAMES, 'linux-asan')
        self.assertEqual(once['failures'], [])
        self.assertEqual(sorted(split['passes']), sorted(once['passes']))
        self.assertEqual(json.loads(json.dumps(split['totals'])), json.loads(json.dumps(once['totals'])))

    def test_a_test_that_never_passed_is_named(self):
        result = runner.audit_attempts([part('test_fx') + part('test_fx_d2', 'cut')], NAMES, 'linux-asan')
        self.assertIn('合格していない試験が残っている: test_fx_d2, test_json', result['failures'])

    def test_failed_then_passed_keeps_the_failure_on_record(self):
        first = part('test_fx') + part('test_fx_d2', 'fail') + part('test_json')
        result = runner.audit_attempts([first, part('test_fx_d2')], NAMES, 'linux-asan')
        self.assertEqual(result['failures'], [])
        rows = result['rerun_after_failure']
        self.assertEqual(list(rows), ['test_fx_d2'])
        self.assertEqual(rows['test_fx_d2'][0]['attempt'], 1)
        self.assertTrue(any('***Failed' in why for why in rows['test_fx_d2'][0]['why']))

    def test_failed_and_never_passed_fails_with_its_reason(self):
        result = runner.audit_attempts([part('test_fx') + part('test_fx_d2', 'fail') + part('test_json'), part('test_fx_d2', 'fail')], NAMES, 'linux-asan')
        self.assertIn('合格していない試験が残っている: test_fx_d2', result['failures'])
        self.assertIn('test_fx_d2: QtTestが成功していない', result['failures'])
        self.assertEqual(result['rerun_after_failure'], {})

    def test_ctest_green_with_a_sanitizer_report_is_not_carried_over(self):
        dirty = part('test_fx', extra='1: ==77==ERROR: AddressSanitizer: heap-use-after-free\n')
        result = runner.audit_attempts([dirty + part('test_fx_d2', 'cut'), part('test_fx_d2') + part('test_json')], NAMES, 'linux-asan')
        self.assertIn('合格していない試験が残っている: test_fx', result['failures'])
        self.assertIn('test_fx: Qt失敗またはsanitizer診断', result['failures'])

    def test_another_tests_report_does_not_spoil_a_clean_one(self):
        # (each test is read by its own numbered lines: test_fx_d2's report is not test_fx's)
        first = part('test_fx') + part('test_fx_d2', 'cut', extra='2: ==77==ERROR: AddressSanitizer: heap-use-after-free\n')
        self.assertEqual(runner.outcomes(first, 'linux-asan')['test_fx'][0], 'passed')
        self.assertEqual(runner.outcomes(first, 'linux-asan')['test_fx_d2'][0], 'unfinished')

    def test_numbers_that_start_alike_are_kept_apart(self):
        names = ['test_a%02d' % i for i in range(1, 13)]
        first = part('test_a01', names=names) + part('test_a12', 'fail', names=names)
        seen = runner.outcomes(first, 'linux-release')
        self.assertEqual(seen['test_a01'][0], 'passed')
        self.assertEqual(seen['test_a12'][0], 'failed')

    def test_missing_or_doubled_totals_are_not_carried_over(self):
        gone = part('test_fx').replace(b'1: Totals: 3 passed, 0 failed, 0 skipped, 0 blacklisted, 12ms\n', b'')
        self.assertEqual(runner.outcomes(gone, 'linux-asan')['test_fx'][0], 'failed')
        twice = part('test_fx').replace(b'1: Totals:', b'1: Totals: 3 passed, 0 failed, 0 skipped\n1: Totals:')
        self.assertEqual(runner.outcomes(twice, 'linux-asan')['test_fx'][0], 'failed')

    def test_a_skip_in_a_carried_test_is_still_a_failure(self):
        skipped = part('test_fx', extra='1: SKIP   : Example::slot() no reference\n').replace(b'0 skipped', b'1 skipped')
        self.assertEqual(runner.outcomes(skipped, 'linux-asan')['test_fx'][0], 'failed')

    def test_a_timeout_is_a_failure(self):
        late = part('test_fx').replace(b'   Passed    0.10 sec', b'***Timeout 300.02 sec')
        self.assertEqual(runner.outcomes(late, 'linux-asan')['test_fx'][0], 'failed')

    def test_running_a_passed_test_again_is_rejected(self):
        result = runner.audit_attempts([part('test_fx') + part('test_fx_d2', 'cut'), part('test_fx') + part('test_fx_d2') + part('test_json')], NAMES, 'linux-asan')
        self.assertIn('2回目: 合格済みの試験を走らせ直している test_fx', result['failures'])

    def test_a_test_outside_the_plan_is_rejected(self):
        result = runner.audit_attempts([b''.join(part(n) for n in NAMES)], NAMES[:2], 'linux-asan')
        self.assertIn('1回目: 計画にない試験 test_json', result['failures'])


class ResumeTests(unittest.TestCase):
    """run() against a made-up build: the three calls that reach ctest and cmake are replaced, the rest is the real code."""

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name) / 'root'
        for directory in ('native', 'tools/ci', 'tools/migration', 'tools/acceptance'):
            (self.root / directory).mkdir(parents=True)
        (self.root / 'native/example.cpp').write_text('int main() {}\n')
        self.build = Path(folder.name) / 'build'
        (self.build / 'tests').mkdir(parents=True)
        for name in NAMES:
            (self.build / 'tests' / name).write_bytes(b'binary of ' + name.encode())
        (self.build / 'CMakeCache.txt').write_text(ConfigurationTests().cache('linux-release'))
        self.folder = Path(folder.name) / 'validation'
        self.out = self.folder / 'linux-release'
        self.plan = {'phase': 'development', 'matrix': {'include': [{'preset': 'linux-release', 'tests': NAMES, 'suite': 'full', 'excluded_contracts': []}]}}
        self.plan_file = Path(folder.name) / 'plan.json'
        self.plan_file.write_text(json.dumps(self.plan))
        self.asked = []

    def go(self, script, resume=True, preset='linux-release', listing=None):
        """One call of run(). script: test name → 'pass' / 'fail' / 'cut'; 'die' = the machine goes after the last line."""
        def listed(build, regex, root):
            return {'tests': [{'name': n, 'command': [str(self.build / 'tests' / n)]} for n in NAMES]}

        def tests_run(build, regex, jobs, root, log):
            names = regex[2:-2].split('|')
            self.asked.append(names)
            failed = 0
            for name in names:
                verdict = script[name]
                log.write(part(name, verdict)); log.flush()
                if verdict == 'cut':
                    raise KeyboardInterrupt  # (the machine is gone: nothing after this line of run() happens)
                failed += verdict == 'fail'
            if script.get('die'):
                raise KeyboardInterrupt
            log.write(f'{100 if not failed else 50}% tests passed, {failed} tests failed out of {len(names)}\n'.encode())
            return 8 if failed else 0

        args = argparse.Namespace(plan=str(self.plan_file), preset=preset, build=str(self.build), output=str(self.out), jobs=2, resume=resume)
        printed = io.StringIO()
        with mock.patch.object(runner, 'list_tests', listing or listed), mock.patch.object(runner, 'build_tests', lambda *a: None), \
                mock.patch.object(runner, 'run_tests', tests_run), contextlib.redirect_stdout(printed):
            code = runner.run(args, root=self.root)
        return code, json.loads(printed.getvalue())

    def cut(self, script, **more):
        with self.assertRaises(KeyboardInterrupt):
            self.go(script, **more)

    def state(self):
        return json.loads((self.out / 'result.json').read_text(encoding='utf-8'))

    def test_a_run_that_was_cut_goes_on_with_the_rest(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'}, resume=False)
        left = self.state()
        self.assertEqual(left['status'], 'running')
        self.assertIn('binary_before', left)
        code, printed = self.go({'test_fx_d2': 'pass', 'test_json': 'pass'})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked[-1], ['test_fx_d2', 'test_json'])
        state = self.state()
        self.assertTrue(state['passed'])
        self.assertEqual(state['resume'], {'used': True, 'carried_over': 1, 'to_run': 2})
        self.assertEqual(printed['resume'], state['resume'])
        self.assertEqual(state['attempt_logs'], ['ctest-output.log', 'ctest-output.attempt-2.log'])
        self.assertEqual(state['attempts'][0]['ended'], 'interrupted')
        self.assertEqual(state['attempts'][0]['passed'], 1)
        self.assertEqual(state['attempts'][0]['unfinished'], ['test_fx_d2'])
        self.assertEqual(state['attempts'][0]['ctest_parallel'], 2)
        self.assertIn('cpus', state['attempts'][0]['environment'])
        self.assertEqual(state['audit']['accepted_in'], {'test_fx': 1, 'test_fx_d2': 2, 'test_json': 2})
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_the_first_runs_log_is_evidence_and_cannot_change(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.go({'test_fx_d2': 'pass', 'test_json': 'pass'})
        first = self.out / 'ctest-output.log'
        first.write_bytes(first.read_bytes() + b'\n')
        self.assertIn('linux-release: raw証跡の欠落・改変', runner.pair_results(self.folder, self.plan)['failures'])
        first.unlink()
        self.assertFalse(runner.pair_results(self.folder, self.plan)['passed'])

    def test_log_names_in_the_record_cannot_point_elsewhere(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.go({'test_fx_d2': 'pass', 'test_json': 'pass'})
        state = self.state()
        state['attempt_logs'] = ['../other/ctest-output.log', 'ctest-output.attempt-2.log']
        (self.out / 'result.json').write_text(json.dumps(state))
        self.assertFalse(runner.pair_results(self.folder, self.plan)['passed'])

    def test_cut_twice_then_finished(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.cut({'test_fx_d2': 'pass', 'test_json': 'cut'})
        code, _ = self.go({'test_json': 'pass'})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked, [NAMES, ['test_fx_d2', 'test_json'], ['test_json']])
        state = self.state()
        self.assertEqual(state['attempt_logs'], ['ctest-output.log', 'ctest-output.attempt-2.log', 'ctest-output.attempt-3.log'])
        self.assertEqual([a['ended'] for a in state['attempts']], ['interrupted', 'interrupted'])
        self.assertEqual(state['audit']['accepted_in'], {'test_fx': 1, 'test_fx_d2': 2, 'test_json': 3})
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_a_failed_test_is_run_again_and_the_failure_stays_on_record(self):
        code, _ = self.go({'test_fx': 'pass', 'test_fx_d2': 'fail', 'test_json': 'pass'})
        self.assertEqual(code, 1)
        self.assertFalse(self.state()['passed'])
        code, printed = self.go({'test_fx_d2': 'pass'})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked[-1], ['test_fx_d2'])
        self.assertEqual(printed['rerun_after_failure'], ['test_fx_d2'])
        state = self.state()
        self.assertEqual(list(state['audit']['rerun_after_failure']), ['test_fx_d2'])
        self.assertEqual(state['attempts'][0]['ended'], 'finished')
        self.assertEqual(list(state['attempts'][0]['failed']), ['test_fx_d2'])
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_a_test_that_fails_again_does_not_pass(self):
        self.go({'test_fx': 'pass', 'test_fx_d2': 'fail', 'test_json': 'pass'})
        code, printed = self.go({'test_fx_d2': 'fail'})
        self.assertEqual(code, 1)
        self.assertIn('合格していない試験が残っている: test_fx_d2', printed['failures'])
        self.assertFalse(runner.pair_results(self.folder, self.plan)['passed'])

    def test_everything_passed_before_the_cut_runs_nothing(self):
        self.cut({n: 'pass' for n in NAMES} | {'die': True})
        code, _ = self.go({})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked, [NAMES])
        state = self.state()
        self.assertEqual(state['resume'], {'used': True, 'carried_over': 3, 'to_run': 0})
        self.assertIsNone(state['log'])
        self.assertEqual(state['attempt_logs'], ['ctest-output.log'])
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def start_over(self, change, reason):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        change()
        code, _ = self.go({n: 'pass' for n in NAMES})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked[-1], NAMES)
        state = self.state()
        self.assertEqual(state['resume'], {'used': False, 'reason': reason})
        self.assertNotIn('attempt_logs', state)
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_changed_source_starts_over(self):
        self.start_over(lambda: (self.root / 'native/example.cpp').write_text('int main() { return 1; }\n'), 'ソースが前回と違う')

    def test_a_changed_tool_starts_over(self):
        self.start_over(lambda: (self.root / 'tools/ci/validation_run.py').write_text('# other\n'), 'ソースが前回と違う')

    def test_a_rebuilt_test_starts_over(self):
        self.start_over(lambda: (self.build / 'tests/test_fx').write_bytes(b'rebuilt'), '実行物が前回と違う')

    def test_another_configuration_starts_over(self):
        cache = self.build / 'CMakeCache.txt'
        self.start_over(lambda: cache.write_text(cache.read_text() + '\nCMAKE_CXX_FLAGS:STRING=-O1'), '構成が前回と違う')

    def test_another_plan_starts_over(self):
        def change():
            self.plan['base'] = 'another'
            self.plan_file.write_text(json.dumps(self.plan))
        self.start_over(change, '計画が前回と違う: plan_hash')

    def test_a_lost_log_starts_over(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.cut({'test_fx_d2': 'pass', 'test_json': 'cut'})
        (self.out / 'ctest-output.log').unlink()
        self.go({n: 'pass' for n in NAMES})
        self.assertEqual(self.asked[-1], NAMES)
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': '前回のCTestログがない'})
        self.assertEqual(sorted(p.name for p in self.out.glob('ctest-output*')), ['ctest-output.log'])

    def test_nothing_passed_before_the_cut_starts_over(self):
        self.cut({'test_fx': 'cut'})
        self.go({n: 'pass' for n in NAMES})
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': '引き継げる合格がない'})

    def test_without_the_flag_every_test_runs_again(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'}, resume=False)
        code, printed = self.go({n: 'pass' for n in NAMES}, resume=False)
        self.assertEqual(code, 0)
        self.assertEqual(self.asked[-1], NAMES)
        self.assertNotIn('resume', printed)
        self.assertNotIn('resume', self.state())

    def test_the_flag_with_no_earlier_record_is_a_plain_run(self):
        code, _ = self.go({n: 'pass' for n in NAMES})
        self.assertEqual(code, 0)
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': '前回の記録がない'})
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_stopping_before_the_decision_keeps_the_earlier_record(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        before = (self.out / 'result.json').read_bytes()

        def broken(build, regex, root):
            raise OSError('ctest could not be started')
        code, printed = self.go({}, listing=broken)
        self.assertEqual(code, 1)
        self.assertFalse(printed['passed'])
        self.assertEqual((self.out / 'result.json').read_bytes(), before)
        code, _ = self.go({'test_fx_d2': 'pass', 'test_json': 'pass'})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked[-1], ['test_fx_d2', 'test_json'])

    def test_a_record_cut_while_written_is_never_half(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.assertEqual([p.name for p in self.out.glob('result.json*')], ['result.json'])
        json.loads((self.out / 'result.json').read_text(encoding='utf-8'))

    def test_windows_is_not_continued(self):
        state = {'preset': 'windows-debug'}
        self.assertEqual(runner.earlier_record(self.out, '{}', state)[1], 'Windows構成の続きからは未対応（QtTestのファイル記録を回ごとに残していない）')

    def test_an_unreadable_record_starts_over(self):
        self.out.mkdir(parents=True)
        (self.out / 'result.json').write_text('{"preset": ')
        self.go({n: 'pass' for n in NAMES})
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': '前回の記録が読めない'})


if __name__ == '__main__':
    unittest.main()
