#!/usr/bin/env python3
"""選択したCTestを一度だけ実行し、同じログで件数・Qt slot・skipを監査する。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
from datetime import datetime, timezone


def audit_log(raw, selected, preset, qt_logs=None):
    failures = []
    starts = re.findall(rb'Start\s+(\d+):\s+(test_\w+)', raw)
    if len(starts) != len(selected) or sorted(n.decode('ascii') for _, n in starts) != sorted(selected):
        failures.append('実行したCTest集合が計画と一致しない')
    wanted = f'100% tests passed, 0 tests failed out of {len(selected)}'.encode()
    if raw.count(wanted) != 1:
        failures.append('CTest完走件数が一致しない')
    if re.search(rb'FAIL!|QFATAL|AddressSanitizer|LeakSanitizer|runtime error:|UndefinedBehaviorSanitizer', raw):
        failures.append('Qt失敗またはsanitizer診断')
    passes = []
    skips = []
    totals = {}
    release = preset.endswith('release')
    known_skips = {
        'TestTransaction': {'failuresAtEachStepRetriedInTheSameProcess', 'lateFailuresAreSettledAtOnce'},
        'TestContractSave': {'oldWriterWhileConverting'},
        'TestSaveE2e': {'crashAtEachStep', 'cutLinesAreKeptApart', 'failureThenTheSameCommandAgain', 'lateFailureIsSettledAtOnce', 'writesWaitForTheRepair', 'undoAndAnotherSaveAfterAFailure'},
        'TestMigrateE2e': {'changedWhileCopied'},
        'TestSession': {'aFailedSaveWritesARecoveryPoint', 'aRetryAfterTheDiskIsBackSavesOnce', 'neitherTheBookNorTheRecoveryAreaCanBeWritten'},
        'TestGuiSave': {'initTestCase'}, 'TestAppE2e': {'initTestCase'},
    }
    for number, name in starts:
        target = name.decode('ascii')
        prefix = rb'(?m)^\s*' + number + rb':\s*'
        test_raw = raw
        if qt_logs is not None:
            test_raw = qt_logs.get(target, b'')
            prefix = rb'(?m)^\s*'
        rows = re.findall(prefix + rb'Totals:\s*(\d+) passed,\s*(\d+) failed,\s*(\d+) skipped', test_raw)
        if len(rows) != 1:
            failures.append(target + ': QtTest Totalsの欠落・重複')
            continue
        counts = tuple(int(n) for n in rows[0])
        totals[target] = counts
        if counts[1] or (not counts[0] and not (release and target in ('test_gui_save', 'test_app_e2e') and counts[2] == 1)):
            failures.append(target + ': QtTestが成功していない')
        if preset.startswith('windows') and target in ('test_gui_layout', 'test_image_split_oom', 'test_app_e2e'):
            expected = (19, 0, 0) if target == 'test_gui_layout' else (7, 0, 0) if target == 'test_image_split_oom' else (0, 0, 1) if release else (8, 0, 0)
            if counts != expected:
                failures.append(target + ': Windows固定条件の件数が不一致')
        pass_rows = re.findall(prefix + rb'PASS\s+:\s+([^\r\n]+)', test_raw)
        passes += [p.hex() for p in pass_rows]
        skip_rows = re.findall(prefix + rb'SKIP\s+:\s+(\w+)::(\w+)(\([^\r\n]*?\))\s+([^\r\n]*)', test_raw)
        if len(skip_rows) != counts[2]:
            failures.append(target + ': SKIP行とTotalsが一致しない')
        for cls, method, args, reason in skip_rows:
            klass, slot = cls.decode('ascii'), method.decode('ascii')
            identity = (cls + b'::' + method + args).hex()
            if not release or slot not in known_skips.get(klass, set()) or b'fault injection' not in reason:
                failures.append(target + ': 予期しないSKIP ' + identity)
            skips.append(identity)
    return {'failures': failures, 'totals': totals, 'passes': passes, 'skips_to_pair': skips,
            'log_sha256': hashlib.sha256(raw).hexdigest(),
            'qt_log_sha256': {name: hashlib.sha256(data).hexdigest() for name, data in (qt_logs or {}).items()}}


def plan_hash(plan):
    return hashlib.sha256(json.dumps(plan, sort_keys=True, ensure_ascii=False).encode('utf-8')).hexdigest()


def configuration_errors(cache, preset):
    values = dict(re.findall(r'^([A-Za-z0-9_]+):[^=\r\n]+=([^\r\n]*)\r?$', cache, re.M))
    expected = {'GENKO_WERROR': 'ON', 'GENKO_FAULT_INJECTION': 'OFF' if preset.endswith('release') else 'ON',
                'CMAKE_BUILD_TYPE': 'Release' if preset.endswith('release') else 'Debug',
                'GENKO_SANITIZE': 'address,undefined' if preset == 'linux-asan' else ''}
    return [f'{preset}: {key}の実構成不一致' for key, value in expected.items() if values.get(key, '') != value]


def source_pins(root):
    result = {}
    for directory in ('native', 'src', 'tools/ci', 'tools/migration', 'tools/acceptance'):
        for path in (root / directory).rglob('*'):
            if path.is_file() and '__pycache__' not in path.parts:
                result[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def binary_pins(commands):
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(set(commands))}


def now():
    return datetime.now(timezone.utc).isoformat()


def run(args):
    root = Path(__file__).resolve().parents[2]
    plan = json.loads(Path(args.plan).read_text(encoding='utf-8'))
    matches = [row for row in plan['matrix']['include'] if row['preset'] == args.preset]
    if len(matches) != 1 or not matches[0]['tests']:
        raise ValueError('構成の選択が空または曖昧です')
    row = matches[0]
    build = Path(args.build).resolve()
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    state = {'preset': args.preset, 'phase': plan['phase'], 'suite': row['suite'], 'selected': row['tests'],
             'excluded_contracts': row['excluded_contracts'], 'started_at': now(), 'status': 'running',
             'source_before': source_pins(root), 'cli23': 'not_run', 'performance': 'not_run', 'plan_hash': plan_hash(plan)}
    result_file = out / 'result.json'
    result_file.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding='utf-8')
    failures = []
    try:
        regex = '^(' + '|'.join(re.escape(n) for n in row['tests']) + ')$'
        registered = json.loads(subprocess.check_output(['ctest', '--test-dir', str(build), '--show-only=json-v1', '-R', regex], cwd=root))
        tests = registered['tests']
        if sorted(t['name'] for t in tests) != row['tests']:
            raise ValueError('登録集合に欠落があります。空試験や未移植を合格にしません')
        (out / 'registered.json').write_text(json.dumps(registered, indent=2), encoding='utf-8')
        with (out / 'build.log').open('wb') as log:
            subprocess.run(['cmake', '--build', str(build), '--parallel', '1', '--verbose', '--target', *row['tests']], cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)
        # 未buildのCTest JSONにはcommandが無い場合があるので、build後に再読込する。
        built = json.loads(subprocess.check_output(['ctest', '--test-dir', str(build), '--show-only=json-v1', '-R', regex], cwd=root))
        commands = [Path(t['command'][0]) for t in built['tests']]
        commands += [p for p in build.rglob('*') if p.is_file() and p.name in ('genko', 'genko.exe', 'genko_lock_helper', 'genko_lock_helper.exe')]
        state['binary_before'] = binary_pins(commands)
        state['configuration'] = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
        config_errors = configuration_errors(state['configuration'], args.preset)
        if config_errors:
            raise ValueError('; '.join(config_errors))
        qtfiles = {t['name']: Path(t['command'][t['command'].index('-o') + 1].removesuffix(',txt'))
                   for t in built['tests'] if '-o' in t['command']}
        for file in qtfiles.values():
            file.unlink(missing_ok=True)  # 前回ログを再利用しない。
        log_file = out / 'ctest-output.log'
        with log_file.open('wb') as log:
            completed = subprocess.run(['ctest', '--test-dir', str(build), '--parallel', '1', '-R', regex, '-V', '--no-tests=error', '--output-on-failure'], cwd=root, stdout=log, stderr=subprocess.STDOUT)
        state['ctest_exit'] = completed.returncode
        qt_logs = None
        if args.preset.startswith('windows'):
            qt_logs = {}
            for name, file in qtfiles.items():
                if file.is_file():
                    data = file.read_bytes(); qt_logs[name] = data
                    (out / file.name).write_bytes(data)
        state['audit'] = audit_log(log_file.read_bytes(), row['tests'], args.preset, qt_logs)
        failures += state['audit']['failures']
        if completed.returncode:
            failures.append('CTest失敗・中断')
        if args.preset.startswith('linux') and (plan['phase'] == 'milestone' or plan['phase'] == 'integration' and row['suite'] == 'full'):
            executable = next(p for p in commands if p.name == 'genko')
            pyref = os.environ.get('GENKO_PYREF')
            if not pyref:
                raise ValueError('CLI23のPython参照が未設定です')
            with (out / 'cli23.log').open('wb') as log:
                p = subprocess.run([pyref, str(root / 'tools/acceptance/m3a_access_check.py'), str(executable)], cwd=root, stdout=log, stderr=subprocess.STDOUT)
            state['cli23'] = 'pass' if p.returncode == 0 else 'fail'
            if p.returncode:
                failures.append('CLI23失敗')
        state['binary_after'] = binary_pins(commands)
        if state['binary_before'] != state['binary_after']:
            failures.append('試験中に実行物が変化')
    except Exception as error:
        failures.append(str(error))
    state['source_after'] = source_pins(root)
    if state['source_before'] != state['source_after']:
        failures.append('試験中にソースが変化')
    state.update(status='finished', finished_at=now(), failures=failures, passed=not failures)
    result_file.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'preset': args.preset, 'count': len(row['tests']), 'passed': not failures, 'failures': failures}, ensure_ascii=False))
    return 1 if failures else 0


def pair_results(folder, plan):
    expected = {r['preset']: r for r in plan['matrix']['include']}
    files = list(Path(folder).rglob('result.json'))
    results = [(p, json.loads(p.read_text(encoding='utf-8'))) for p in files]
    rows = {r['preset']: r for _, r in results}
    failures = []
    if sorted(rows) != sorted(expected) or len(rows) != len(results):
        failures.append('構成artifactの欠落・重複')
    for path, row in results:
        preset = row['preset']; wanted = expected.get(preset)
        if not wanted:
            continue
        if not row.get('passed') or row.get('status') != 'finished' or row.get('failures') or row.get('ctest_exit') != 0:
            failures.append(preset + ': 構成未合格')
        if row.get('plan_hash') != plan_hash(plan) or row.get('phase') != plan['phase'] or row.get('suite') != wanted['suite'] or row.get('selected') != wanted['tests'] or row.get('excluded_contracts') != wanted['excluded_contracts']:
            failures.append(preset + ': 計画と検証範囲の不一致')
        failures += configuration_errors(row.get('configuration', ''), preset)
        for kind in ('source', 'binary'):
            before, after = row.get(kind + '_before'), row.get(kind + '_after')
            if not before or before != after or not all(re.fullmatch(r'[0-9a-f]{64}', v) for v in before.values()):
                failures.append(preset + ': ' + kind + ' pinsの欠落・不一致')
        try:
            registered = json.loads((path.parent / 'registered.json').read_text(encoding='utf-8'))
            if sorted(t['name'] for t in registered['tests']) != wanted['tests']:
                raise ValueError('登録集合不一致')
            (path.parent / 'build.log').read_bytes()
            raw = (path.parent / 'ctest-output.log').read_bytes()
            qt_logs = None
            if preset.startswith('windows'):
                qt_logs = {n: (path.parent / ('qtest-' + n + '.txt')).read_bytes() for n in wanted['tests']}
            audit = audit_log(raw, wanted['tests'], preset, qt_logs)
            if json.dumps(audit, sort_keys=True) != json.dumps(row.get('audit'), sort_keys=True):
                failures.append(preset + ': raw証跡の欠落・改変')
            failures += [preset + ': ' + f for f in audit['failures']]
            cli_required = preset.startswith('linux') and (plan['phase'] == 'milestone' or plan['phase'] == 'integration' and wanted['suite'] == 'full')
            if cli_required:
                if row.get('cli23') != 'pass' or not (path.parent / 'cli23.log').is_file():
                    failures.append(preset + ': CLI23証跡の欠落')
        except (OSError, ValueError, KeyError) as error:
            failures.append(preset + ': 必須証跡の欠落・不正 ' + str(error))
        if preset.endswith('release'):
            counterpart = rows.get('windows-debug' if preset.startswith('windows') else 'linux-asan', {})
            passed = set(counterpart.get('audit', {}).get('passes', []))
            for slot in row.get('audit', {}).get('skips_to_pair', []):
                if slot not in passed:
                    failures.append(preset + ': 対照PASSがない ' + slot)
    for platform in ('linux', 'windows'):
        pins = [r.get('source_before') for n, r in rows.items() if n.startswith(platform)]
        if pins and any(p != pins[0] for p in pins):
            failures.append(platform + ': 構成間のsource不一致')
    return {'passed': not failures, 'failures': failures, 'presets': sorted(rows), 'performance': 'not_run'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', required=True)
    parser.add_argument('--preset')
    parser.add_argument('--build')
    parser.add_argument('--output', required=True)
    parser.add_argument('--pair', action='store_true')
    args = parser.parse_args()
    if args.pair:
        plan = json.loads(Path(args.plan).read_text(encoding='utf-8'))
        result = pair_results(args.output, plan)
        (Path(args.output) / 'matrix-audit.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(result, ensure_ascii=False))
        return 0 if result['passed'] else 1
    if not args.preset or not args.build:
        parser.error('--presetと--buildが必要です')
    return run(args)


if __name__ == '__main__':
    sys.exit(main())
