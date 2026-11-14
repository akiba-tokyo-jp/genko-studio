"""ログ収集を再実行にせず、一回のCTest記録を監査する。"""
import importlib.util
from pathlib import Path
import unittest
import json
import argparse
import contextlib
import hashlib
import io
import os
import sys
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
DONE = '100%% tests passed, 0 tests failed out of %d\n'
BROKEN = 'FAIL!  : Example::broken() compared values differ'
try:
    import fcntl
except ImportError:
    fcntl = None


def part(name, verdict='pass', names=NAMES, extra=''):
    """One test's share of a ctest -V log: its start, its own numbered lines, and (unless it was cut) its result line."""
    number = names.index(name) + 1
    text = f'    Start {number}: {name}\n{number}: PASS   : Example::{name}()\n{extra}'
    if verdict == 'cut':
        return text.encode()
    failed = 1 if verdict == 'fail' else 0
    if failed:
        text += f'{number}: {BROKEN}\n'
    text += f'{number}: Totals: 3 passed, {failed} failed, 0 skipped, 0 blacklisted, 12ms\n'
    text += f'{number}/{len(names)} Test #{number}: {name} ' + '.' * 20 + ('***Failed    0.10 sec\n' if failed else '   Passed    0.10 sec\n')
    return text.encode()


def done(count):
    return (DONE % count).encode()


class AttemptTests(unittest.TestCase):
    def test_a_cut_run_and_the_rest_pass_together(self):
        first = part('test_fx') + part('test_fx_d2', 'cut')
        second = part('test_fx_d2') + part('test_json') + done(2)
        result = runner.audit_attempts([first, second], NAMES, 'linux-asan', ['ctest-output.log', 'ctest-output.attempt-2.log'])
        self.assertEqual(result['failures'], [])
        self.assertEqual(result['accepted_in'], {'test_fx': 'ctest-output.log', 'test_fx_d2': 'ctest-output.attempt-2.log', 'test_json': 'ctest-output.attempt-2.log'})
        self.assertEqual(result['rerun_after_failure'], {})
        self.assertEqual(sorted(result['totals']), NAMES)
        self.assertEqual(len(result['passes']), 3)
        self.assertEqual(result['attempt_log_sha256'], [hashlib.sha256(first).hexdigest(), hashlib.sha256(second).hexdigest()])

    def test_the_same_slots_as_one_whole_run(self):
        # (carried over or not, the release-with-asan pairing reads the same PASS rows)
        whole = b''.join(part(n) for n in NAMES) + done(3)
        split = runner.audit_attempts([part('test_fx') + part('test_fx_d2', 'cut'), part('test_fx_d2') + part('test_json') + done(2)], NAMES, 'linux-asan')
        once = runner.audit_log(whole, NAMES, 'linux-asan')
        self.assertEqual(once['failures'], [])
        self.assertEqual(split['failures'], [])
        self.assertEqual(sorted(split['passes']), sorted(once['passes']))
        self.assertEqual(json.loads(json.dumps(split['totals'])), json.loads(json.dumps(once['totals'])))

    def test_a_test_that_never_passed_is_named(self):
        result = runner.audit_attempts([part('test_fx') + part('test_fx_d2', 'cut')], NAMES, 'linux-asan')
        self.assertIn('合格していない試験が残っている: test_fx_d2, test_json', result['failures'])

    def test_the_last_run_must_say_it_ran_to_the_end(self):
        first = part('test_fx') + part('test_fx_d2', 'cut')
        rest = part('test_fx_d2') + part('test_json')
        self.assertEqual(runner.audit_attempts([first, rest + done(2)], NAMES, 'linux-asan')['failures'], [])
        self.assertEqual(runner.audit_attempts([first, rest], NAMES, 'linux-asan')['failures'], ['CTest完走件数が一致しない'])
        self.assertEqual(runner.audit_attempts([first, rest + done(2) + done(2)], NAMES, 'linux-asan')['failures'], ['CTest完走件数が一致しない'])
        self.assertEqual(runner.audit_attempts([first, rest + done(3)], NAMES, 'linux-asan')['failures'], ['CTest完走件数が一致しない'])

    def test_failed_then_passed_keeps_the_failure_on_record(self):
        first = part('test_fx') + part('test_fx_d2', 'fail') + part('test_json')
        result = runner.audit_attempts([first, part('test_fx_d2') + done(1)], NAMES, 'linux-asan', ['ctest-output.log', 'ctest-output.attempt-2.log'])
        self.assertEqual(result['failures'], [])
        rows = result['rerun_after_failure']
        self.assertEqual(list(rows), ['test_fx_d2'])
        self.assertEqual(rows['test_fx_d2'][0]['log'], 'ctest-output.log')
        self.assertTrue(any('***Failed' in why for why in rows['test_fx_d2'][0]['why']))

    def test_cut_after_its_own_failure_is_kept_as_a_failure(self):
        # (a test stopped after it had already reported: a later pass must not leave no trace)
        for extra in ('2: ==77==ERROR: AddressSanitizer: heap-use-after-free\n', '2: Totals: 2 passed, 1 failed, 0 skipped, 0 blacklisted, 5ms\n'):
            first = part('test_fx') + part('test_fx_d2', 'cut', extra=extra)
            self.assertEqual(runner.outcomes(first, 'linux-asan')['test_fx_d2'][0], 'failed')
            result = runner.audit_attempts([first, part('test_fx_d2') + part('test_json') + done(2)], NAMES, 'linux-asan')
            self.assertEqual(result['failures'], [])
            self.assertEqual(list(result['rerun_after_failure']), ['test_fx_d2'])

    def test_failed_and_never_passed_fails_with_its_reason(self):
        result = runner.audit_attempts([part('test_fx') + part('test_fx_d2', 'fail') + part('test_json'), part('test_fx_d2', 'fail')], NAMES, 'linux-asan')
        self.assertIn('合格していない試験が残っている: test_fx_d2', result['failures'])
        self.assertIn('test_fx_d2: QtTestが成功していない', result['failures'])
        self.assertEqual(result['rerun_after_failure'], {})

    def test_ctest_green_with_a_sanitizer_report_is_not_carried_over(self):
        dirty = part('test_fx', extra='1: ==77==ERROR: AddressSanitizer: heap-use-after-free\n')
        result = runner.audit_attempts([dirty + part('test_fx_d2', 'cut'), part('test_fx_d2') + part('test_json') + done(2)], NAMES, 'linux-asan')
        self.assertIn('合格していない試験が残っている: test_fx', result['failures'])
        self.assertIn('test_fx: Qt失敗またはsanitizer診断', result['failures'])

    def test_a_report_behind_a_carriage_return_is_seen(self):
        # (ctest cuts lines at \n only: a progress line that ends in \r hides nothing)
        dirty = part('test_fx', extra='1: loading 50%\r x.cpp:3:5: runtime error: signed integer overflow\n')
        self.assertEqual(runner.outcomes(dirty, 'linux-asan')['test_fx'][0], 'failed')
        self.assertIn('Qt失敗またはsanitizer診断', runner.audit_log(dirty + done(1), ['test_fx'], 'linux-asan')['failures'])

    def test_another_tests_report_does_not_spoil_a_clean_one(self):
        # (each test is read by its own numbered lines: test_fx_d2's report is not test_fx's)
        first = part('test_fx') + part('test_fx_d2', 'cut', extra='2: ==77==ERROR: AddressSanitizer: heap-use-after-free\n')
        self.assertEqual(runner.outcomes(first, 'linux-asan')['test_fx'][0], 'passed')
        self.assertEqual(runner.outcomes(first, 'linux-asan')['test_fx_d2'][0], 'failed')

    def test_numbers_that_start_alike_are_kept_apart(self):
        names = ['test_a%02d' % i for i in range(1, 22)]
        for other in ('test_a11', 'test_a21', 'test_a12'):
            first = part('test_a01', names=names) + part(other, 'fail', names=names)
            seen = runner.outcomes(first, 'linux-release')
            self.assertEqual(seen['test_a01'][0], 'passed', other)
            self.assertEqual(seen[other][0], 'failed')
        # (and the other way: the report is test 1's, test 11 is clean)
        first = part('test_a01', 'fail', names=names) + part('test_a11', names=names)
        self.assertEqual(runner.outcomes(first, 'linux-release')['test_a11'][0], 'passed')
        self.assertEqual(runner.outcomes(first, 'linux-release')['test_a01'][0], 'failed')

    def test_names_that_start_alike_are_kept_apart(self):
        # (the result line of test_fx_d2 is not test_fx's)
        first = part('test_fx', 'cut') + part('test_fx_d2')
        seen = runner.outcomes(first, 'linux-asan')
        self.assertEqual(seen['test_fx'][0], 'unfinished')
        self.assertEqual(seen['test_fx_d2'][0], 'passed')

    def test_a_result_line_for_another_name_is_not_a_pass(self):
        wrong = part('test_fx').replace(b'Test #1: test_fx ', b'Test #1: test_other ')
        self.assertEqual(runner.outcomes(wrong, 'linux-asan')['test_fx'][0], 'failed')

    def test_two_result_lines_are_not_a_pass(self):
        twice = part('test_fx') + b'1/3 Test #1: test_fx ....................   Passed    0.10 sec\n'
        self.assertEqual(runner.outcomes(twice, 'linux-asan')['test_fx'][0], 'failed')

    def test_started_twice_is_not_a_pass(self):
        self.assertEqual(runner.outcomes(part('test_fx') + part('test_fx'), 'linux-asan')['test_fx'][0], 'failed')

    def test_missing_or_doubled_totals_are_not_carried_over(self):
        gone = part('test_fx').replace(b'1: Totals: 3 passed, 0 failed, 0 skipped, 0 blacklisted, 12ms\n', b'')
        self.assertEqual(runner.outcomes(gone, 'linux-asan')['test_fx'][0], 'failed')
        twice = part('test_fx').replace(b'1: Totals:', b'1: Totals: 3 passed, 0 failed, 0 skipped\n1: Totals:')
        self.assertEqual(runner.outcomes(twice, 'linux-asan')['test_fx'][0], 'failed')

    def test_a_skip_in_a_carried_test_is_still_a_failure(self):
        skipped = part('test_fx', extra='1: SKIP   : Example::slot() no reference\n').replace(b'0 skipped', b'1 skipped')
        self.assertEqual(runner.outcomes(skipped, 'linux-asan')['test_fx'][0], 'failed')

    def test_results_that_are_not_passed(self):
        for verdict in (b'***Timeout 300.02 sec', b'***Not Run   0.00 sec', b'***Skipped   0.00 sec', b'***Exception: SegFault  0.10 sec', b'Subprocess aborted***Exception:   0.10 sec'):
            late = part('test_fx').replace(b'   Passed    0.10 sec', verdict)
            self.assertEqual(runner.outcomes(late, 'linux-asan')['test_fx'][0], 'failed', verdict)

    def test_windows_line_ends_are_read(self):
        first = (part('test_fx') + part('test_fx_d2', 'cut')).replace(b'\n', b'\r\n')
        self.assertEqual(runner.outcomes(first, 'linux-asan')['test_fx'][0], 'passed')

    def test_running_a_passed_test_again_is_rejected(self):
        result = runner.audit_attempts([part('test_fx') + part('test_fx_d2', 'cut'), b''.join(part(n) for n in NAMES) + done(3)], NAMES, 'linux-asan')
        self.assertIn('2回目: 合格済みの試験を走らせ直している test_fx', result['failures'])

    def test_a_test_outside_the_plan_is_rejected(self):
        result = runner.audit_attempts([b''.join(part(n) for n in NAMES) + done(3)], NAMES[:2], 'linux-asan')
        self.assertIn('1回目: 計画にない試験 test_json', result['failures'])

    def test_a_report_that_belongs_to_no_test_fails_the_record(self):
        # (what the whole-log check of a single run catches must not slip through when the logs are read test by test)
        clean = b''.join(part(n) for n in NAMES) + done(3)
        for line in (b'x.cpp:3:5: runtime error: signed integer overflow\n', b'9: ==77==ERROR: AddressSanitizer: heap-use-after-free\n', b'QFATAL : somewhere\n'):
            result = runner.audit_attempts([clean + line], NAMES, 'linux-asan')
            self.assertEqual(result['failures'], ['1回目: どの試験のものか分からないQt失敗またはsanitizer診断'], line)
            self.assertTrue(runner.audit_log(clean + line, NAMES, 'linux-asan')['failures'])
        self.assertEqual(runner.audit_attempts([clean], NAMES, 'linux-asan')['failures'], [])

    def test_the_copy_of_a_failed_tests_output_is_its_own(self):
        # (--output-on-failure prints a failed test's lines again without their number)
        first = part('test_fx') + part('test_fx_d2', 'fail') + part('test_json') + (BROKEN + '\n').encode()
        result = runner.audit_attempts([first, part('test_fx_d2') + done(1)], NAMES, 'linux-asan')
        self.assertEqual(result['failures'], [])
        # (the same line with no failed test to own it is nobody's)
        alone = b''.join(part(n) for n in NAMES) + (BROKEN + '\n').encode() + done(3)
        self.assertTrue(runner.audit_attempts([alone], NAMES, 'linux-asan')['failures'])


class RecordTests(unittest.TestCase):
    def test_a_record_cut_while_written_keeps_the_one_before(self):
        with tempfile.TemporaryDirectory() as folder:
            file = Path(folder) / 'result.json'
            runner.write_state(file, {'attempt': 1})
            with mock.patch.object(runner.os, 'replace', side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt):
                runner.write_state(file, {'attempt': 2, 'much': 'x' * 100000})
            self.assertEqual(json.loads(file.read_text(encoding='utf-8')), {'attempt': 1})
            runner.write_state(file, {'attempt': 3})
            self.assertEqual(json.loads(file.read_text(encoding='utf-8')), {'attempt': 3})

    def test_a_record_held_open_for_a_moment_is_written_after_it(self):
        with tempfile.TemporaryDirectory() as folder:
            file = Path(folder) / 'result.json'
            real = os.replace
            calls = []

            def busy(a, b):
                calls.append(a)
                if len(calls) < 3:
                    raise PermissionError('in use')
                real(a, b)
            with mock.patch.object(runner.os, 'replace', busy), mock.patch.object(runner.time, 'sleep', lambda s: None):
                runner.write_state(file, {'attempt': 1})
            self.assertEqual(json.loads(file.read_text(encoding='utf-8')), {'attempt': 1})


class ResumeTests(unittest.TestCase):
    """run() against a made-up build: the three calls that reach ctest and cmake are replaced, the rest is the real code."""

    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.root = Path(folder.name).resolve() / 'root'
        for directory in ('native', 'tools/ci', 'tools/migration', 'tools/acceptance'):
            (self.root / directory).mkdir(parents=True)
        (self.root / 'native/example.cpp').write_text('int main() {}\n')
        # (the access check of a stage's run: here it passes unless the file beside it says no)
        (self.root / 'tools/acceptance/m3a_access_check.py').write_text('import pathlib, sys\nprint("checked", sys.argv[1])\nsys.exit(1 if pathlib.Path(sys.argv[1]).with_name("refuse").exists() else 0)\n')
        self.build = Path(folder.name).resolve() / 'build'
        (self.build / 'tests').mkdir(parents=True)
        (self.build / 'src').mkdir()
        for name in NAMES:
            (self.build / 'tests' / name).write_bytes(b'binary of ' + name.encode())
        (self.build / 'genko').write_bytes(b'the product')
        (self.build / 'src/libgenko_core.so').write_bytes(b'the product code of a sanitizer build')
        (self.build / 'CMakeCache.txt').write_text(ConfigurationTests().cache('linux-release'))
        self.folder = Path(folder.name).resolve() / 'validation'
        self.out = self.folder / 'linux-release'
        self.plan = {'phase': 'development', 'matrix': {'include': [{'preset': 'linux-release', 'tests': NAMES, 'suite': 'full', 'excluded_contracts': []}]}}
        self.plan_file = Path(folder.name) / 'plan.json'
        self.plan_file.write_text(json.dumps(self.plan))
        self.asked = []

    def go(self, script, resume=True, listing=None, writing=None):
        """One call of run(). script: test name → 'pass' / 'fail' / 'cut'; 'die' = the machine goes after the last test;
        'loose' = text ctest prints with no test's number; 'unsaid' = ctest does not say it ran to the end."""
        def listed(build, regex, root):
            return {'tests': [{'name': n, 'command': [str(self.build / 'tests' / n)]} for n in NAMES]}

        def tests_run(build, regex, jobs, root, log, keep=()):
            names = regex[2:-2].split('|')
            self.asked.append(names)
            if script.get('refused'):
                raise OSError('ctest could not be started')
            log.write(f'Test project {build}\n'.encode())
            failed = 0
            for name in names:
                verdict = script[name]
                log.write(part(name, verdict)); log.flush()
                if verdict == 'cut':
                    raise KeyboardInterrupt  # (the machine is gone: nothing after this line of run() happens)
                failed += verdict == 'fail'
            log.write(script.get('loose', '').encode())
            if script.get('die'):
                raise KeyboardInterrupt
            if failed:
                log.write(f'50% tests passed, {failed} tests failed out of {len(names)}\n'.encode())
            elif not script.get('unsaid'):
                log.write(done(len(names)))
            return 8 if failed else 0

        args = argparse.Namespace(plan=str(self.plan_file), preset='linux-release', build=str(self.build), output=str(self.out), jobs=2, resume=resume)
        printed = io.StringIO()
        with mock.patch.object(runner, 'list_tests', listing or listed), mock.patch.object(runner, 'build_tests', lambda *a: None), \
                mock.patch.object(runner, 'run_tests', tests_run), mock.patch.object(runner, 'write_state', writing or runner.write_state), \
                mock.patch.dict(os.environ, {'GENKO_PYREF': sys.executable}), contextlib.redirect_stdout(printed):
            code = runner.run(args, root=self.root)
        return code, json.loads(printed.getvalue())

    def cut(self, script, **more):
        with self.assertRaises(KeyboardInterrupt):
            self.go(script, **more)

    def state(self):
        return json.loads((self.out / 'result.json').read_text(encoding='utf-8'))

    def edit(self, change):
        state = self.state()
        change(state)
        (self.out / 'result.json').write_text(json.dumps(state), encoding='utf-8')

    def logs(self):
        return sorted(p.name for p in self.out.glob('ctest-output*'))

    def everything(self):
        return {n: 'pass' for n in NAMES}

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
        self.assertEqual(state['audit']['accepted_in'], {'test_fx': 'ctest-output.log', 'test_fx_d2': 'ctest-output.attempt-2.log', 'test_json': 'ctest-output.attempt-2.log'})
        paired = runner.pair_results(self.folder, self.plan)
        self.assertTrue(paired['passed'], paired)
        self.assertEqual(paired['resumed'], {'linux-release': {'attempts': 2, 'rerun_after_failure': [], 'earlier_failures': []}})

    def test_the_first_runs_log_is_evidence_and_cannot_change(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.go({'test_fx_d2': 'pass', 'test_json': 'pass'})
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])
        first = self.out / 'ctest-output.log'
        kept = first.read_bytes()
        first.write_bytes(kept + b'\n')
        self.assertFalse(runner.pair_results(self.folder, self.plan)['passed'])
        # (and with the record of that run made to agree with the changed log, the audit on record still does not)
        self.edit(lambda state: state['attempts'][0].update(log_sha256=hashlib.sha256(kept + b'\n').hexdigest()))
        self.assertIn('linux-release: raw証跡の欠落・改変', runner.pair_results(self.folder, self.plan)['failures'])
        first.unlink()
        self.assertFalse(runner.pair_results(self.folder, self.plan)['passed'])

    def test_the_list_of_logs_cannot_be_cut_short_or_point_elsewhere(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.go({'test_fx_d2': 'pass', 'test_json': 'pass'})
        kept = (self.out / 'result.json').read_bytes()
        other = self.folder / 'other'; other.mkdir()
        (other / 'ctest-output.log').write_bytes((self.out / 'ctest-output.log').read_bytes())
        self.edit(lambda state: state.update(attempt_logs=['../other/ctest-output.log', 'ctest-output.attempt-2.log']))
        self.assertTrue(any('各回のログ名が不正' in f or 'CTestログが記録と合わない' in f for f in runner.pair_results(self.folder, self.plan)['failures']))
        (self.out / 'result.json').write_bytes(kept)
        self.edit(lambda state: state.update(attempt_logs=['ctest-output.attempt-2.log']))
        self.assertFalse(runner.pair_results(self.folder, self.plan)['passed'])
        (self.out / 'result.json').write_bytes(kept)
        (self.out / 'ctest-output.attempt-9.log').write_bytes(b'a log the record does not name')
        self.assertTrue(any('CTestログが記録と合わない' in f for f in runner.pair_results(self.folder, self.plan)['failures']))
        (self.out / 'ctest-output.attempt-9.log').unlink()
        self.edit(lambda state: state['attempts'][0].update(log_sha256='0' * 64))
        self.assertTrue(any('各回の記録とログが合わない' in f for f in runner.pair_results(self.folder, self.plan)['failures']))
        (self.out / 'result.json').write_bytes(kept)
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_cut_twice_then_finished(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.cut({'test_fx_d2': 'pass', 'test_json': 'cut'})
        code, _ = self.go({'test_json': 'pass'})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked, [NAMES, ['test_fx_d2', 'test_json'], ['test_json']])
        state = self.state()
        self.assertEqual(state['attempt_logs'], ['ctest-output.log', 'ctest-output.attempt-2.log', 'ctest-output.attempt-3.log'])
        self.assertEqual([a['ended'] for a in state['attempts']], ['interrupted', 'interrupted'])
        self.assertEqual([a['log'] for a in state['attempts']], ['ctest-output.log', 'ctest-output.attempt-2.log'])
        self.assertEqual(state['audit']['accepted_in'], {'test_fx': 'ctest-output.log', 'test_fx_d2': 'ctest-output.attempt-2.log', 'test_json': 'ctest-output.attempt-3.log'})
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_a_failed_test_is_run_again_and_the_failure_stays_on_record(self):
        code, _ = self.go({'test_fx': 'pass', 'test_fx_d2': 'fail', 'test_json': 'pass', 'loose': BROKEN + '\n'})
        self.assertEqual(code, 1)
        self.assertFalse(self.state()['passed'])
        code, printed = self.go({'test_fx_d2': 'pass'})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked[-1], ['test_fx_d2'])
        self.assertEqual(printed['rerun_after_failure'], ['test_fx_d2'])
        self.assertIn('CTest失敗・中断', printed['earlier_failures'])
        state = self.state()
        self.assertEqual(list(state['audit']['rerun_after_failure']), ['test_fx_d2'])
        self.assertEqual(state['attempts'][0]['ended'], 'finished')
        self.assertEqual(list(state['attempts'][0]['failed']), ['test_fx_d2'])
        paired = runner.pair_results(self.folder, self.plan)
        self.assertTrue(paired['passed'], paired)
        self.assertEqual(paired['resumed']['linux-release']['rerun_after_failure'], ['test_fx_d2'])

    def test_a_test_that_fails_again_does_not_pass(self):
        self.go({'test_fx': 'pass', 'test_fx_d2': 'fail', 'test_json': 'pass'})
        code, printed = self.go({'test_fx_d2': 'fail'})
        self.assertEqual(code, 1)
        self.assertIn('合格していない試験が残っている: test_fx_d2', printed['failures'])
        self.assertFalse(runner.pair_results(self.folder, self.plan)['passed'])
        code, _ = self.go({'test_fx_d2': 'pass'})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked, [NAMES, ['test_fx_d2'], ['test_fx_d2']])
        self.assertEqual([r['log'] for r in self.state()['audit']['rerun_after_failure']['test_fx_d2']], ['ctest-output.log', 'ctest-output.attempt-2.log'])
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_cut_after_ctest_said_it_was_done_runs_nothing(self):
        # (the machine went between ctest's end and the record: e.g. during the access check)
        self.cut(self.everything() | {'loose': DONE % 3, 'die': True})
        code, _ = self.go({})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked, [NAMES])
        state = self.state()
        self.assertEqual(state['resume'], {'used': True, 'carried_over': 3, 'to_run': 0})
        self.assertIsNone(state['log'])
        self.assertEqual(state['ctest_exit'], 0)
        self.assertEqual(state['attempt_logs'], ['ctest-output.log'])
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_every_test_passed_but_ctest_never_said_done_starts_over(self):
        self.cut(self.everything() | {'die': True})
        self.go(self.everything())
        self.assertEqual(self.asked, [NAMES, NAMES])
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': '全部合格しているが、最後の回のCTestが走り切ったと書いていない'})
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_a_finished_run_that_only_lacked_ctests_last_word_is_not_made_a_pass(self):
        code, _ = self.go(self.everything() | {'unsaid': True}, resume=False)
        self.assertEqual(code, 1)
        code, _ = self.go(self.everything())
        self.assertEqual(self.asked, [NAMES, NAMES])
        self.assertEqual(code, 0)

    def test_a_finished_run_with_a_report_of_no_test_is_not_made_a_pass(self):
        code, printed = self.go(self.everything() | {'loose': 'x.cpp:3:5: runtime error: signed integer overflow\n'}, resume=False)
        self.assertEqual(code, 1)
        self.assertEqual(printed['failures'], ['Qt失敗またはsanitizer診断'])
        code, _ = self.go(self.everything())
        self.assertEqual(self.asked, [NAMES, NAMES])
        self.assertTrue(self.state()['resume']['reason'].startswith('前回のログが監査に通らない: '))

    def test_a_pass_is_left_as_it_is(self):
        code, _ = self.go(self.everything(), resume=False)
        self.assertEqual(code, 0)
        before = (self.out / 'result.json').read_bytes()
        code, printed = self.go({})
        self.assertEqual(code, 0)
        self.assertEqual(printed['resume'], {'used': True, 'carried_over': 3, 'to_run': 0, 'unchanged': True})
        self.assertEqual(self.asked, [NAMES])
        self.assertEqual((self.out / 'result.json').read_bytes(), before)

    def test_a_pass_of_another_source_runs_again(self):
        self.go(self.everything(), resume=False)
        (self.root / 'native/example.cpp').write_text('int main() { return 1; }\n')
        self.go(self.everything())
        self.assertEqual(self.asked, [NAMES, NAMES])
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': 'ソースが前回と違う'})

    def test_a_pass_does_not_outlive_a_run_that_could_not_start(self):
        # (a wrapper reads the record: a build that broke after a pass must not leave the pass there)
        self.go(self.everything(), resume=False)

        def broken(build, regex, root):
            raise OSError('ctest could not be started')
        code, printed = self.go({}, listing=broken)
        self.assertEqual(code, 1)
        self.assertFalse(self.state()['passed'])
        self.assertNotIn('ctest_exit', self.state())

    def start_over(self, change, reason, first=None):
        self.cut(first or {'test_fx': 'pass', 'test_fx_d2': 'cut'})
        change()
        code, _ = self.go(self.everything())
        self.assertEqual(code, 0)
        self.assertEqual(self.asked[-1], NAMES)
        state = self.state()
        self.assertEqual(state['resume'], {'used': False, 'reason': reason})
        self.assertNotIn('attempt_logs', state)
        self.assertEqual(self.logs(), ['ctest-output.log'])
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_changed_source_starts_over(self):
        self.start_over(lambda: (self.root / 'native/example.cpp').write_text('int main() { return 1; }\n'), 'ソースが前回と違う')

    def test_a_changed_tool_starts_over(self):
        self.start_over(lambda: (self.root / 'tools/ci/validation_run.py').write_text('# other\n'), 'ソースが前回と違う')

    def test_source_that_changed_during_the_earlier_run_starts_over(self):
        self.go({'test_fx': 'pass', 'test_fx_d2': 'fail', 'test_json': 'pass'})
        self.edit(lambda state: state['source_after'].update({'native/example.cpp': '0' * 64}))
        self.go(self.everything())
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': 'ソースが前回と違う'})

    def test_a_rebuilt_test_starts_over(self):
        self.start_over(lambda: (self.build / 'tests/test_fx').write_bytes(b'rebuilt'), '実行物が前回と違う')

    def test_rebuilt_product_code_in_a_shared_library_starts_over(self):
        # (a sanitizer build keeps the product's code in libgenko_*.so: the test executables alone do not show a change)
        self.start_over(lambda: (self.build / 'src/libgenko_core.so').write_bytes(b'rebuilt'), '実行物が前回と違う')

    def test_binaries_that_changed_during_the_earlier_run_start_over(self):
        self.go({'test_fx': 'pass', 'test_fx_d2': 'fail', 'test_json': 'pass'})
        self.edit(lambda state: state['binary_after'].update({str(self.build / 'tests/test_fx'): '0' * 64}))
        self.go(self.everything())
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': '実行物が前回と違う'})

    def test_a_record_without_the_binaries_starts_over(self):
        self.start_over(lambda: self.edit(lambda state: state.pop('binary_before')), '前回の記録に実行物の記録がない')

    def test_a_record_of_the_tool_before_starts_over(self):
        self.start_over(lambda: self.edit(lambda state: state.pop('log')), '前回の記録にログ名がない（続きからに対応する前の道具の記録）')

    def test_another_configuration_starts_over(self):
        cache = self.build / 'CMakeCache.txt'
        self.start_over(lambda: cache.write_text(cache.read_text() + '\nCMAKE_CXX_FLAGS:STRING=-O1'), '構成が前回と違う')

    def test_another_plan_starts_over(self):
        def change():
            self.plan['base'] = 'another'
            self.plan_file.write_text(json.dumps(self.plan))
        self.start_over(change, '計画が前回と違う: plan_hash')

    def test_a_record_for_another_selection_starts_over(self):
        for key, value in (('preset', 'linux-asan'), ('phase', 'integration'), ('suite', 'related'), ('selected', NAMES[:2]), ('excluded_contracts', ['x'])):
            with self.subTest(key=key):
                self.setUp()
                self.start_over(lambda: self.edit(lambda state: state.update({key: value})), '計画が前回と違う: ' + key)

    def test_a_record_that_cannot_be_read_starts_over(self):
        for text in (b'{"preset": ', b'\xff\xfe not text', b'[]', b'{"attempts": null}'):
            with self.subTest(text=text):
                self.setUp()
                self.start_over(lambda: (self.out / 'result.json').write_bytes(text), '前回の記録が読めない')

    def test_log_names_in_the_record_that_point_elsewhere_start_over(self):
        def change():
            other = self.folder / 'other'; other.mkdir()
            (other / 'ctest-output.log').write_bytes((self.out / 'ctest-output.log').read_bytes())
            self.edit(lambda state: state.update(attempt_logs=['../other/ctest-output.log']))
        self.start_over(change, '前回の記録のログ名が不正')

    def test_a_log_of_another_build_starts_over(self):
        def change():
            log = self.out / 'ctest-output.log'
            log.write_bytes(log.read_bytes().replace(str(self.build).encode(), b'/somewhere/else'))
        self.start_over(change, '前回のCTestログがこのビルドのものでない: ctest-output.log')

    def test_a_lost_log_starts_over(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.cut({'test_fx_d2': 'pass', 'test_json': 'cut'})
        (self.out / 'ctest-output.log').unlink()
        self.go(self.everything())
        self.assertEqual(self.asked[-1], NAMES)
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': '前回のCTestログがない'})
        self.assertEqual(self.logs(), ['ctest-output.log'])

    def test_a_log_that_changed_after_it_was_recorded_starts_over(self):
        # (e.g. a ctest that outlived the tool and went on writing)
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.cut({'test_fx_d2': 'pass', 'test_json': 'cut'})
        first = self.out / 'ctest-output.log'
        first.write_bytes(first.read_bytes() + part('test_fx_d2'))
        self.go(self.everything())
        self.assertEqual(self.asked[-1], NAMES)
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': '前回のCTestログが記録と違う: ctest-output.log'})

    def test_a_finished_runs_log_that_changed_starts_over(self):
        self.go({'test_fx': 'pass', 'test_fx_d2': 'fail', 'test_json': 'pass'})
        first = self.out / 'ctest-output.log'
        first.write_bytes(first.read_bytes().replace(b'***Failed', b'   Passed').replace(b'1 failed', b'0 failed').replace(BROKEN.encode(), b''))
        self.go(self.everything())
        self.assertEqual(self.asked[-1], NAMES)
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': '前回のCTestログが記録と違う: ctest-output.log'})

    def test_a_run_again_that_broke_the_record_starts_over(self):
        # (nothing on record says the first log changed: the logs themselves do not agree)
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.cut({'test_fx_d2': 'pass', 'test_json': 'cut'})
        first = self.out / 'ctest-output.log'
        first.write_bytes(f'Test project {self.build}\n'.encode() + part('test_fx') + part('test_fx_d2'))
        self.edit(lambda state: state['attempts'][0].update(log_sha256=hashlib.sha256(first.read_bytes()).hexdigest()))
        self.go(self.everything())
        self.assertEqual(self.asked[-1], NAMES)
        self.assertTrue(self.state()['resume']['reason'].startswith('前回のログが監査に通らない: ctest-output.attempt-2.log: 合格済みの試験を走らせ直している'))

    def test_nothing_passed_before_the_cut_starts_over(self):
        self.start_over(lambda: None, '引き継げる合格がない', first={'test_fx': 'cut'})

    def test_a_stale_log_is_never_taken_for_this_runs(self):
        # (a run that starts over and is cut before ctest opens its log: the log of the source before must not be there
        # for the next run to carry over)
        self.go({'test_fx': 'pass', 'test_fx_d2': 'fail', 'test_json': 'pass'})
        (self.root / 'native/example.cpp').write_text('int main() { return 1; }\n')

        real = runner.write_state

        def cut_after_the_record(file, state):
            real(file, state)
            if state.get('status') == 'running' and 'binary_before' in state:
                raise KeyboardInterrupt
        for resume in (True, False):
            with self.assertRaises(KeyboardInterrupt):
                self.go(self.everything(), resume=resume, writing=cut_after_the_record)
            self.assertEqual(self.logs(), [])
        self.assertEqual(len(self.asked), 1)
        code, _ = self.go(self.everything())
        self.assertEqual(code, 0)
        self.assertEqual(self.asked[-1], NAMES)
        self.assertEqual(self.state()['resume'], {'used': False, 'reason': '引き継げる合格がない'})

    def test_a_run_again_cut_before_its_log_goes_on_from_the_logs_before(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        real = runner.write_state

        def cut_after_the_record(file, state):
            real(file, state)
            raise KeyboardInterrupt
        with self.assertRaises(KeyboardInterrupt):
            self.go({}, writing=cut_after_the_record)
        self.assertEqual(self.logs(), ['ctest-output.log'])
        code, _ = self.go({'test_fx_d2': 'pass', 'test_json': 'pass'})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked[-1], ['test_fx_d2', 'test_json'])
        state = self.state()
        self.assertEqual(state['attempt_logs'], ['ctest-output.log', 'ctest-output.attempt-3.log'])
        self.assertEqual([a['log'] for a in state['attempts']], ['ctest-output.log', None])
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_ctest_that_could_not_start_keeps_what_was_carried(self):
        for lost in (False, True):
            with self.subTest(lost=lost):
                self.setUp()
                self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
                code, printed = self.go({'refused': True})
                self.assertEqual(code, 1)
                self.assertIn('ctest could not be started', printed['failures'])
                self.assertNotIn('ctest_exit', self.state())
                self.assertEqual(self.state()['log'], 'ctest-output.attempt-2.log')
                if lost:
                    (self.out / 'ctest-output.attempt-2.log').unlink()  # (as when the disk was full: the log was never made)
                code, _ = self.go({'test_fx_d2': 'pass', 'test_json': 'pass'})
                self.assertEqual(code, 0)
                self.assertEqual(self.asked[-1], ['test_fx_d2', 'test_json'])
                self.assertEqual(self.state()['attempt_logs'], ['ctest-output.log', 'ctest-output.attempt-3.log'])
                self.assertEqual(self.logs(), ['ctest-output.attempt-3.log', 'ctest-output.log'])
                self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_without_the_flag_every_test_runs_again(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'}, resume=False)
        code, printed = self.go(self.everything(), resume=False)
        self.assertEqual(code, 0)
        self.assertEqual(self.asked[-1], NAMES)
        self.assertNotIn('resume', printed)
        self.assertNotIn('resume', self.state())
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_without_the_flag_logs_of_a_run_before_are_cleared(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.cut({'test_fx_d2': 'pass', 'test_json': 'cut'})
        self.assertEqual(self.logs(), ['ctest-output.attempt-2.log', 'ctest-output.log'])
        self.go(self.everything(), resume=False)
        self.assertEqual(self.logs(), ['ctest-output.log'])
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_the_flag_with_no_earlier_record_is_a_plain_run(self):
        code, _ = self.go(self.everything())
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

    def milestone(self):
        self.plan['phase'] = 'milestone'
        self.plan_file.write_text(json.dumps(self.plan))

    def test_the_access_check_runs_with_the_last_run(self):
        self.milestone()
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.assertEqual(self.state()['cli23'], 'not_run')
        code, _ = self.go({'test_fx_d2': 'pass', 'test_json': 'pass'})
        self.assertEqual(code, 0)
        self.assertEqual(self.state()['cli23'], 'pass')
        self.assertIn(b'checked', (self.out / 'cli23.log').read_bytes())
        self.assertTrue(runner.pair_results(self.folder, self.plan)['passed'])

    def test_a_failed_access_check_is_run_again_and_stays_on_record(self):
        self.milestone()
        (self.build / 'refuse').write_text('no')
        code, printed = self.go(self.everything())
        self.assertEqual(code, 1)
        self.assertEqual(printed['failures'], ['CLI23失敗'])
        (self.build / 'refuse').unlink()
        code, printed = self.go({})
        self.assertEqual(code, 0)
        self.assertEqual(self.asked, [NAMES])
        self.assertEqual(printed['earlier_failures'], ['CLI23失敗'])
        state = self.state()
        self.assertEqual(state['cli23'], 'pass')
        self.assertEqual(state['attempts'][0]['cli23'], 'fail')
        self.assertTrue((self.out / 'cli23.attempt-1.log').is_file())
        paired = runner.pair_results(self.folder, self.plan)
        self.assertTrue(paired['passed'], paired)
        self.assertEqual(paired['resumed']['linux-release']['earlier_failures'], ['CLI23失敗'])

    @unittest.skipUnless(fcntl, 'the folder is held with flock, which this system does not have')
    def test_processes_left_from_the_run_before_keep_the_next_one_out(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        before = (self.out / 'result.json').read_bytes()
        left = os.open(self.out / '.running', os.O_RDWR)  # (a ctest or a test that outlived the tool holds this)
        fcntl.flock(left, fcntl.LOCK_EX | fcntl.LOCK_NB)
        try:
            code, printed = self.go({})
        finally:
            os.close(left)
        self.assertEqual(code, 1)
        self.assertEqual(printed['failures'], ['この出力先を使う前の実行がまだ動いている（ctestか試験のプロセスが残っている）'])
        self.assertEqual(self.asked, [NAMES])
        self.assertEqual((self.out / 'result.json').read_bytes(), before)
        code, _ = self.go({'test_fx_d2': 'pass', 'test_json': 'pass'})
        self.assertEqual(code, 0)

    def test_windows_is_not_continued(self):
        state = {'preset': 'windows-debug'}
        self.assertEqual(runner.earlier_record(self.out, b'{}', state, self.build)[1], 'Windows構成の続きからは未対応（QtTestのファイル記録を回ごとに残していない）')

    def test_a_windows_record_with_several_logs_is_refused(self):
        self.cut({'test_fx': 'pass', 'test_fx_d2': 'cut'})
        self.go({'test_fx_d2': 'pass', 'test_json': 'pass'})
        self.edit(lambda state: state.update(preset='windows-release'))
        self.plan['matrix']['include'][0]['preset'] = 'windows-release'
        self.assertTrue(any('各回のログ名が不正' in f for f in runner.pair_results(self.folder, self.plan)['failures']))


if __name__ == '__main__':
    unittest.main()
