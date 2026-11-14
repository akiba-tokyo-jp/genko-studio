#!/usr/bin/env python3
"""選択したCTestを実行し、そのログで件数・Qt slot・skipを監査する。

--resume を付けると、同じ出力先に残った前回の記録（途中で止まった回・落ちた回）から続ける。
計画・ソース・実行物・構成が前回と同じときだけ、合格済みの試験の結果を引き継ぎ、残りと落ちた分だけを走らせる。
どれかが違えば、引き継がずに最初から走らせる。各回のCTestログは消さずに残し、その全部を監査する。
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import subprocess
import sys
from datetime import datetime, timezone


DIAGNOSTICS = rb'FAIL!|QFATAL|AddressSanitizer|LeakSanitizer|runtime error:|UndefinedBehaviorSanitizer'
KNOWN_SKIPS = {
    'TestTransaction': {'failuresAtEachStepRetriedInTheSameProcess', 'lateFailuresAreSettledAtOnce'},
    'TestContractSave': {'oldWriterWhileConverting'},
    'TestSaveE2e': {'crashAtEachStep', 'cutLinesAreKeptApart', 'failureThenTheSameCommandAgain', 'lateFailureIsSettledAtOnce', 'writesWaitForTheRepair', 'undoAndAnotherSaveAfterAFailure'},
    'TestMigrateE2e': {'changedWhileCopied'},
    'TestSession': {'aFailedSaveWritesARecoveryPoint', 'aRetryAfterTheDiskIsBackSavesOnce', 'neitherTheBookNorTheRecoveryAreaCanBeWritten'},
    'TestGuiSave': {'initTestCase'}, 'TestAppE2e': {'initTestCase'},
}
ATTEMPT_LOG = re.compile(r'ctest-output(\.attempt-[0-9]+)?\.log')


def audit_one(target, test_raw, prefix, preset):
    """一つの試験のQtTest出力を調べる。返すのは (不合格の理由, Totals または None, PASS行, SKIP行)。"""
    release = preset.endswith('release')
    rows = re.findall(prefix + rb'Totals:\s*(\d+) passed,\s*(\d+) failed,\s*(\d+) skipped', test_raw)
    if len(rows) != 1:
        return [target + ': QtTest Totalsの欠落・重複'], None, [], []
    failures = []
    counts = tuple(int(n) for n in rows[0])
    if counts[1] or (not counts[0] and not (release and target in ('test_gui_save', 'test_app_e2e') and counts[2] == 1)):
        failures.append(target + ': QtTestが成功していない')
    if preset.startswith('windows') and target in ('test_gui_layout', 'test_image_split_oom', 'test_app_e2e'):
        expected = (19, 0, 0) if target == 'test_gui_layout' else (7, 0, 0) if target == 'test_image_split_oom' else (0, 0, 1) if release else (8, 0, 0)
        if counts != expected:
            failures.append(target + ': Windows固定条件の件数が不一致')
    passes = [p.hex() for p in re.findall(prefix + rb'PASS\s+:\s+([^\r\n]+)', test_raw)]
    skips = []
    skip_rows = re.findall(prefix + rb'SKIP\s+:\s+(\w+)::(\w+)(\([^\r\n]*?\))\s+([^\r\n]*)', test_raw)
    if len(skip_rows) != counts[2]:
        failures.append(target + ': SKIP行とTotalsが一致しない')
    for cls, method, args, reason in skip_rows:
        klass, slot = cls.decode('ascii'), method.decode('ascii')
        identity = (cls + b'::' + method + args).hex()
        if not release or slot not in KNOWN_SKIPS.get(klass, set()) or b'fault injection' not in reason:
            failures.append(target + ': 予期しないSKIP ' + identity)
        skips.append(identity)
    return failures, counts, passes, skips


def audit_log(raw, selected, preset, qt_logs=None):
    """一回で走り切ったCTestログを監査する。"""
    failures = []
    starts = re.findall(rb'Start\s+(\d+):\s+(test_\w+)', raw)
    if len(starts) != len(selected) or sorted(n.decode('ascii') for _, n in starts) != sorted(selected):
        failures.append('実行したCTest集合が計画と一致しない')
    wanted = f'100% tests passed, 0 tests failed out of {len(selected)}'.encode()
    if raw.count(wanted) != 1:
        failures.append('CTest完走件数が一致しない')
    if re.search(DIAGNOSTICS, raw):
        failures.append('Qt失敗またはsanitizer診断')
    passes = []
    skips = []
    totals = {}
    for number, name in starts:
        target = name.decode('ascii')
        prefix = rb'(?m)^\s*' + number + rb':\s*'
        test_raw = raw
        if qt_logs is not None:
            test_raw = qt_logs.get(target, b'')
            prefix = rb'(?m)^\s*'
        found, counts, test_passes, test_skips = audit_one(target, test_raw, prefix, preset)
        failures += found
        if counts is None:
            continue
        totals[target] = counts
        passes += test_passes
        skips += test_skips
    return {'failures': failures, 'totals': totals, 'passes': passes, 'skips_to_pair': skips,
            'log_sha256': hashlib.sha256(raw).hexdigest(),
            'qt_log_sha256': {name: hashlib.sha256(data).hexdigest() for name, data in (qt_logs or {}).items()}}


def outcomes(raw, preset):
    """一回分のCTestログ（途中で切れていてもよい）から、始まった試験ごとの結末を読む。

    返すのは 試験名 → ('passed', [], その試験の件数など) / ('failed', 理由, None) / ('unfinished', [], None)。
    合格は、CTestの結果行が Passed で、その試験自身の行にQt失敗もsanitizer診断もなく、Totals・SKIPが正しいときだけ。
    """
    result = {}
    for number, name in re.findall(rb'Start\s+(\d+):\s+(test_\w+)', raw):
        target = name.decode('ascii')
        if target in result:
            result[target] = ('failed', [target + ': 同じ回に二度始まっている'], None)
            continue
        rows = re.findall(rb'(?m)^\s*\d+/\d+\s+Test\s+#' + number + rb':\s+' + re.escape(name) + rb'\s+\.*([^\r\n]*)', raw)
        if not rows:
            result[target] = ('unfinished', [], None)
            continue
        why = []
        if len(rows) != 1 or not re.fullmatch(rb'\s+Passed\s+[\d.]+\s+sec\s*', rows[0]):
            why.append(target + ': CTestの結果 ' + ' / '.join(r.strip().decode('utf-8', 'replace') for r in rows))
        prefix = rb'(?m)^\s*' + number + rb':\s*'
        if re.search(prefix + rb'[^\r\n]*(?:' + DIAGNOSTICS + rb')', raw):
            why.append(target + ': Qt失敗またはsanitizer診断')
        found, counts, passes, skips = audit_one(target, raw, prefix, preset)
        why += found
        result[target] = ('failed', why, None) if why else ('passed', [], {'totals': counts, 'passes': passes, 'skips': skips})
    return result


def audit_attempts(logs, selected, preset):
    """続きから走らせた記録を監査する。logs は古い順の各回のCTestログ。

    計画の試験はどれも、いずれか一回で合格していなければならない。合格した試験を後の回で走らせ直してはいけない。
    前の回で落ちて後の回で合格した試験は rerun_after_failure に、落ちた回と理由を残す（原因はHANDOFFに書く）。
    """
    failures = []
    accepted = {}
    data = {}
    earlier = {}
    wanted = set(selected)
    for index, raw in enumerate(logs, 1):
        for name, (verdict, why, detail) in outcomes(raw, preset).items():
            if name not in wanted:
                failures.append(f'{index}回目: 計画にない試験 {name}')
            elif name in accepted:
                failures.append(f'{index}回目: 合格済みの試験を走らせ直している {name}')
            elif verdict == 'passed':
                accepted[name] = index
                data[name] = detail
            elif verdict == 'failed':
                earlier.setdefault(name, []).append({'attempt': index, 'why': why})
    missing = [name for name in selected if name not in accepted]
    if missing:
        failures.append('合格していない試験が残っている: ' + ', '.join(missing))
        for name in missing:
            failures += [w for row in earlier.get(name, [])[-1:] for w in row['why']]
    return {'failures': failures,
            'totals': {name: data[name]['totals'] for name in selected if name in data},
            'passes': [p for name in selected if name in data for p in data[name]['passes']],
            'skips_to_pair': [s for name in selected if name in data for s in data[name]['skips']],
            'log_sha256': hashlib.sha256(logs[-1]).hexdigest() if logs else '',
            'qt_log_sha256': {},
            'attempt_log_sha256': [hashlib.sha256(raw).hexdigest() for raw in logs],
            'accepted_in': {name: accepted[name] for name in selected if name in accepted},
            'rerun_after_failure': {name: rows for name, rows in sorted(earlier.items()) if name in accepted}}


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
    for directory in ('native', 'tools/ci', 'tools/migration', 'tools/acceptance'):
        for path in (root / directory).rglob('*'):
            if path.is_file() and '__pycache__' not in path.parts:
                result[str(path.relative_to(root))] = hashlib.sha256(path.read_bytes()).hexdigest()
    return result


def binary_pins(commands):
    return {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(set(commands))}


def now():
    return datetime.now(timezone.utc).isoformat()


def environment():
    """走らせた機械（利用者指示: 実行環境を結果に残す。続きからの回は機械が替わっていることがある）。"""
    info = {'system': platform.system(), 'release': platform.release(), 'machine': platform.machine(), 'cpus': os.cpu_count(), 'memory_bytes': None}
    try:
        info['memory_bytes'] = os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES')
    except (AttributeError, ValueError, OSError):
        pass
    return info


def write_state(file, state):
    # (written beside and moved into place: a machine that dies while writing must not leave half a record, the
    # next --resume reads it)
    part = file.with_name(file.name + '.part')
    part.write_text(json.dumps(state, ensure_ascii=False, indent=2), encoding='utf-8')
    os.replace(part, file)


def list_tests(build, regex, root):
    return json.loads(subprocess.check_output(['ctest', '--test-dir', str(build), '--show-only=json-v1', '-R', regex], cwd=root))


def build_tests(build, tests, root, log):
    subprocess.run(['cmake', '--build', str(build), '--parallel', '1', '--verbose', '--target', *tests], cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)


def run_tests(build, regex, jobs, root, log):
    # (-V prefixes each line with its test's number, so the tests run side by side are still read one by one)
    return subprocess.run(['ctest', '--test-dir', str(build), '--parallel', str(jobs), '-R', regex, '-V', '--no-tests=error', '--output-on-failure'], cwd=root, stdout=log, stderr=subprocess.STDOUT).returncode


def earlier_record(out, old_text, state):
    """--resume: 前回の記録から続けてよいかを調べる。返すのは (前回の記録, None) か (None, 続けない理由)。"""
    if old_text is None:
        return None, '前回の記録がない'
    try:
        old = json.loads(old_text)
    except ValueError:
        return None, '前回の記録が読めない'
    if not isinstance(old, dict):
        return None, '前回の記録が読めない'
    if state['preset'].startswith('windows'):
        return None, 'Windows構成の続きからは未対応（QtTestのファイル記録を回ごとに残していない）'
    for key in ('preset', 'phase', 'suite', 'selected', 'excluded_contracts', 'plan_hash'):
        if old.get(key) != state[key]:
            return None, '計画が前回と違う: ' + key
    if not old.get('source_before') or old['source_before'] != state['source_before'] or old.get('source_after', old['source_before']) != old['source_before']:
        return None, 'ソースが前回と違う'
    if not old.get('binary_before'):
        return None, '前回の記録に実行物の記録がない'
    if old['binary_before'] != state['binary_before'] or old.get('binary_after', old['binary_before']) != old['binary_before']:
        return None, '実行物が前回と違う'
    if old.get('configuration') != state['configuration']:
        return None, '構成が前回と違う'
    names = list(old.get('attempt_logs') or ([old['log']] if old.get('log') else []))
    if any(not isinstance(n, str) or not ATTEMPT_LOG.fullmatch(n) for n in names):
        return None, '前回の記録のログ名が不正'
    # (the last one may be missing: the machine died before ctest opened it)
    if names and old.get('status') != 'finished' and not (out / names[-1]).is_file():
        names.pop()
    if any(not (out / n).is_file() for n in names):
        return None, '前回のCTestログがない'
    old['attempt_logs'] = names
    return old, None


def attempt_summary(old, raw, preset):
    verdicts = outcomes(raw, preset) if raw is not None else {}
    return {'started_at': old.get('started_at'), 'finished_at': old.get('finished_at'),
            'ended': 'finished' if old.get('status') == 'finished' else 'interrupted',
            'ctest_parallel': old.get('ctest_parallel'), 'ctest_exit': old.get('ctest_exit'),
            'environment': old.get('environment'), 'log': old.get('log') if raw is not None else None,
            'log_sha256': hashlib.sha256(raw).hexdigest() if raw is not None else None,
            'passed': sum(1 for v in verdicts.values() if v[0] == 'passed'),
            'failed': {name: v[1] for name, v in sorted(verdicts.items()) if v[0] == 'failed'},
            'unfinished': sorted(name for name, v in verdicts.items() if v[0] == 'unfinished'),
            'failures': old.get('failures', [])}


def run(args, root=None):
    root = Path(root).resolve() if root else Path(__file__).resolve().parents[2]
    plan = json.loads(Path(args.plan).read_text(encoding='utf-8'))
    matches = [row for row in plan['matrix']['include'] if row['preset'] == args.preset]
    if len(matches) != 1 or not matches[0]['tests']:
        raise ValueError('構成の選択が空または曖昧です')
    row = matches[0]
    build = Path(args.build).resolve()
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    result_file = out / 'result.json'
    resume = getattr(args, 'resume', False)
    old_text = result_file.read_text(encoding='utf-8') if resume and result_file.is_file() else None
    state = {'preset': args.preset, 'phase': plan['phase'], 'suite': row['suite'], 'selected': row['tests'],
             'excluded_contracts': row['excluded_contracts'], 'started_at': now(), 'status': 'running',
             'source_before': source_pins(root), 'cli23': 'not_run', 'performance': 'not_run', 'plan_hash': plan_hash(plan),
             'ctest_parallel': args.jobs, 'environment': environment(), 'log': 'ctest-output.log'}
    if old_text is None:
        write_state(result_file, state)  # (with a record to resume from, that record stays until this run knows whether it continues it)
    failures = []
    decided = old_text is None
    try:
        regex = '^(' + '|'.join(re.escape(n) for n in row['tests']) + ')$'
        registered = list_tests(build, regex, root)
        tests = registered['tests']
        if sorted(t['name'] for t in tests) != row['tests']:
            raise ValueError('登録集合に欠落があります。空試験や未移植を合格にしません')
        (out / 'registered.json').write_text(json.dumps(registered, indent=2), encoding='utf-8')
        with (out / 'build.log').open('wb') as log:
            build_tests(build, row['tests'], root, log)
        # 未buildのCTest JSONにはcommandが無い場合があるので、build後に再読込する。
        built = list_tests(build, regex, root)
        commands = [Path(t['command'][0]) for t in built['tests']]
        commands += [p for p in build.rglob('*') if p.is_file() and p.name in ('genko', 'genko.exe', 'genko_lock_helper', 'genko_lock_helper.exe')]
        state['binary_before'] = binary_pins(commands)
        state['configuration'] = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
        config_errors = configuration_errors(state['configuration'], args.preset)
        if config_errors:
            raise ValueError('; '.join(config_errors))
        to_run = list(row['tests'])
        earlier_logs = []
        if resume:
            old, reason = earlier_record(out, old_text, state)
            carried = {}
            if old is not None:
                earlier_logs = old['attempt_logs']
                carried = audit_attempts([(out / n).read_bytes() for n in earlier_logs], row['tests'], args.preset)['accepted_in']
                if not carried:
                    old, reason = None, '引き継げる合格がない'
            if old is None:
                earlier_logs = []
                state['resume'] = {'used': False, 'reason': reason}
            else:
                own = old.get('log') if old.get('log') in earlier_logs else None
                state['attempts'] = list(old.get('attempts', [])) + [attempt_summary(old, (out / own).read_bytes() if own else None, args.preset)]
                to_run = [n for n in row['tests'] if n not in carried]
                state['log'] = f"ctest-output.attempt-{len(state['attempts']) + 1}.log" if to_run else None
                state['attempt_logs'] = earlier_logs + ([state['log']] if to_run else [])
                state['resume'] = {'used': True, 'carried_over': len(carried), 'to_run': len(to_run)}
        decided = True
        if not earlier_logs:
            for stale in out.glob('ctest-output.attempt-*.log'):
                stale.unlink()
        qtfiles = {t['name']: Path(t['command'][t['command'].index('-o') + 1].removesuffix(',txt'))
                   for t in built['tests'] if '-o' in t['command']}
        for file in qtfiles.values():
            file.unlink(missing_ok=True)  # 前回ログを再利用しない。
        # (the pins and the configuration are on disk before the tests start: a run that dies in the tests can be
        # continued only against them)
        write_state(result_file, state)
        log_file = out / 'ctest-output.log'
        state['ctest_exit'] = 0
        if to_run:
            log_file = out / state['log']
            with log_file.open('wb') as log:
                state['ctest_exit'] = run_tests(build, '^(' + '|'.join(re.escape(n) for n in to_run) + ')$', args.jobs, root, log)
        qt_logs = None
        if args.preset.startswith('windows'):
            qt_logs = {}
            for name, file in qtfiles.items():
                if file.is_file():
                    data = file.read_bytes(); qt_logs[name] = data
                    (out / file.name).write_bytes(data)
        if earlier_logs:
            state['audit'] = audit_attempts([(out / n).read_bytes() for n in state['attempt_logs']], row['tests'], args.preset)
        else:
            state['audit'] = audit_log(log_file.read_bytes(), row['tests'], args.preset, qt_logs)
        failures += state['audit']['failures']
        if state['ctest_exit']:
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
    if not decided:
        # 続けるかどうかを決める前に止まった（登録・ビルド・構成の失敗）。前回の記録は書き換えずに残す。
        print(json.dumps({'preset': args.preset, 'count': len(row['tests']), 'passed': False, 'failures': failures,
                          'resume': {'used': False, 'reason': '続きからの判定の前に止まった。前回の記録は残してある'}}, ensure_ascii=False))
        return 1
    state['source_after'] = source_pins(root)
    if state['source_before'] != state['source_after']:
        failures.append('試験中にソースが変化')
    state.update(status='finished', finished_at=now(), failures=failures, passed=not failures)
    write_state(result_file, state)
    summary = {'preset': args.preset, 'count': len(row['tests']), 'passed': not failures, 'failures': failures}
    if state.get('resume'):
        summary['resume'] = state['resume']
        summary['rerun_after_failure'] = sorted(state.get('audit', {}).get('rerun_after_failure', {}))
    print(json.dumps(summary, ensure_ascii=False))
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
            attempt_logs = row.get('attempt_logs')
            if attempt_logs is not None:
                # 続きから走らせた記録: 各回のログを全部読み直す。
                if not isinstance(attempt_logs, list) or not attempt_logs or any(not isinstance(n, str) or not ATTEMPT_LOG.fullmatch(n) for n in attempt_logs):
                    raise ValueError('各回のログ名が不正')
                audit = audit_attempts([(path.parent / n).read_bytes() for n in attempt_logs], wanted['tests'], preset)
            else:
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
    for platform_name in ('linux', 'windows'):
        pins = [r.get('source_before') for n, r in rows.items() if n.startswith(platform_name)]
        if pins and any(p != pins[0] for p in pins):
            failures.append(platform_name + ': 構成間のsource不一致')
    return {'passed': not failures, 'failures': failures, 'presets': sorted(rows), 'performance': 'not_run'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--plan', required=True)
    parser.add_argument('--preset')
    parser.add_argument('--build')
    parser.add_argument('--output', required=True)
    parser.add_argument('--pair', action='store_true')
    # 試験を同時に流す数（利用者決定: 試験の資源上限と並列度は固定しない。result.json の ctest_parallel に残す）
    parser.add_argument('--jobs', type=int, default=1)
    # 同じ出力先の前回の記録から続ける（利用者決定 2026-11-14）。計画・ソース・実行物・構成が同じときだけ引き継ぐ。
    parser.add_argument('--resume', action='store_true')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobsは1以上です')
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
