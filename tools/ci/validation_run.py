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
import time
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


def read_log(raw, preset):
    """一回分のCTestログ（途中で切れていてもよい）を一度だけ読む。返すのは (試験ごとの結末, 持ち主の分からない診断の行)。

    結末は 試験名 → ('passed', [], その試験の件数など) / ('failed', 理由, None) / ('unfinished', [], None)。
    合格は、CTestの結果行が一つだけで Passed、その試験自身の行にQt失敗もsanitizer診断もなく、Totals・SKIPが正しいときだけ。
    """
    starts = re.findall(rb'Start\s+(\d+):\s+(test_\w+)', raw)
    own = {}
    rows = {}
    loose = []
    # (ctest cuts its lines at \n only: a \r inside a test's line does not start another line)
    for line in raw.split(b'\n'):
        numbered = re.match(rb'\s*(\d+):', line)
        if numbered:
            own.setdefault(numbered.group(1), []).append(line)
            continue
        row = re.match(rb'\s*\d+/\d+\s+Test\s+#(\d+):\s+(\S+)\s+\.*(.*)', line, re.S)
        if row:
            rows.setdefault(row.group(1), []).append((row.group(2), row.group(3)))
        elif re.search(DIAGNOSTICS, line):
            loose.append(line)
    result = {}
    for number, name in starts:
        target = name.decode('ascii')
        if target in result:
            result[target] = ('failed', [target + ': 同じ回に二度始まっている'], None)
            continue
        text = b'\n'.join(own.get(number, [])) + b'\n'
        why = [target + ': Qt失敗またはsanitizer診断'] if re.search(DIAGNOSTICS, text) else []
        found, counts, passes, skips = audit_one(target, text, rb'(?m)^\s*' + number + rb':\s*', preset)
        ended = rows.get(number, [])
        if not ended:
            # 走っている途中で切れた試験。そこまでの出力に失敗が出ていれば、落ちた試験として残す。
            if why or (counts is not None and counts[1]):
                result[target] = ('failed', why + (found if counts is not None else []), None)
            else:
                result[target] = ('unfinished', [], None)
            continue
        if len(ended) != 1 or ended[0][0] != name or not re.fullmatch(rb'\s*Passed\s+[\d.]+\s+sec\s*', ended[0][1]):
            why.insert(0, target + ': CTestの結果 ' + ' / '.join((n + b' ' + v.strip()).decode('utf-8', 'replace') for n, v in ended))
        why += found
        result[target] = ('failed', why, None) if why else ('passed', [], {'totals': counts, 'passes': passes, 'skips': skips})
    # 番号の付かない診断の行は、落ちた・終わらなかった試験の出力の写し（--output-on-failure）だけを認める。
    numbers = {number: name.decode('ascii') for number, name in starts}
    excused = set()
    stray = []
    for number, lines in own.items():
        if number not in numbers:
            stray += [line for line in lines if re.search(DIAGNOSTICS, line)]
        elif result[numbers[number]][0] != 'passed':
            excused.update(re.sub(rb'^\s*\d+:', b'', line).strip() for line in lines)
    stray += [line for line in loose if line.strip() not in excused]
    return result, stray


def outcomes(raw, preset):
    return read_log(raw, preset)[0]


def ran_to_the_end(raw):
    """CTestが、始めた試験を全部合格で走り切ったと自分で書いているか。"""
    count = len(re.findall(rb'Start\s+(\d+):\s+(test_\w+)', raw))
    return raw.count(f'100% tests passed, 0 tests failed out of {count}'.encode()) == 1


def walk_attempts(logs, selected, preset, names):
    """各回のログを古い順にたどる。返すのは (記録そのものの不備, 試験 → 合格した回のログ名, 件数など, 落ちた回)。"""
    broken = []
    accepted = {}
    data = {}
    earlier = {}
    wanted = set(selected)
    for label, raw in zip(names, logs):
        verdicts, stray = read_log(raw, preset)
        if stray:
            broken.append(f'{label}: どの試験のものか分からないQt失敗またはsanitizer診断')
        for name, (verdict, why, detail) in verdicts.items():
            if name not in wanted:
                broken.append(f'{label}: 計画にない試験 {name}')
            elif name in accepted:
                broken.append(f'{label}: 合格済みの試験を走らせ直している {name}')
            elif verdict == 'passed':
                accepted[name] = label
                data[name] = detail
            elif verdict == 'failed':
                earlier.setdefault(name, []).append({'log': label, 'why': why})
    return broken, accepted, data, earlier


def audit_attempts(logs, selected, preset, names=None):
    """続きから走らせた記録を監査する。logs は古い順の各回のCTestログ、names はそのファイル名。

    計画の試験はどれも、いずれか一回で合格していなければならない。合格した試験を後の回で走らせ直してはいけない。
    最後の回は、始めた試験を全部合格で走り切っていなければならない。
    前の回で落ちて後の回で合格した試験は rerun_after_failure に、落ちた回と理由を残す（原因はHANDOFFに書く）。
    """
    names = list(names) if names is not None else [f'{i}回目' for i in range(1, len(logs) + 1)]
    failures, accepted, data, earlier = walk_attempts(logs, selected, preset, names)
    missing = [name for name in selected if name not in accepted]
    if missing:
        failures.append('合格していない試験が残っている: ' + ', '.join(missing))
        for name in missing:
            failures += [w for row in earlier.get(name, [])[-1:] for w in row['why']]
    if not logs or not ran_to_the_end(logs[-1]):
        failures.append('CTest完走件数が一致しない')
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


def binaries(build, built):
    """試験が動かす実行物: 各試験、genko と排他の補助、ビルドの中の共有ライブラリ（sanitizer の構成では製品のコードはここにある）。"""
    commands = [Path(t['command'][0]) for t in built['tests']]
    commands += [p for p in build.rglob('*') if p.is_file() and (p.name in ('genko', 'genko.exe', 'genko_lock_helper', 'genko_lock_helper.exe') or p.suffix in ('.so', '.dll', '.dylib'))]
    return commands


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
    # (written beside, flushed to the disk and moved into place: a machine that dies while writing must not leave half
    # a record, the next --resume reads it)
    part = file.with_name(file.name + '.part')
    with part.open('w', encoding='utf-8') as out:
        out.write(json.dumps(state, ensure_ascii=False, indent=2))
        out.flush()
        os.fsync(out.fileno())
    for wait in (0.1, 0.3, 1.0, None):
        try:
            os.replace(part, file)
            break
        except PermissionError:  # (Windows: another program has the record open for a moment)
            if wait is None:
                raise
            time.sleep(wait)
    try:
        folder = os.open(file.parent, os.O_RDONLY)
    except OSError:
        return
    try:
        os.fsync(folder)
    except OSError:
        pass
    finally:
        os.close(folder)


def hold(out):
    """出力先を一つの実行だけが使う。返すのは握った印（無い環境では None）。

    印はctestと試験のプロセスにも渡る。道具だけが落ちてctestや試験が生き残っているあいだは、次の回が同じビルドで
    重ねて走らないよう、握れない。
    """
    try:
        import fcntl
    except ImportError:
        return None
    lock = os.open(out / '.running', os.O_RDWR | os.O_CREAT, 0o644)
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        os.close(lock)
        raise RuntimeError('この出力先を使う前の実行がまだ動いている（ctestか試験のプロセスが残っている）')
    return lock


def list_tests(build, regex, root):
    return json.loads(subprocess.check_output(['ctest', '--test-dir', str(build), '--show-only=json-v1', '-R', regex], cwd=root))


def build_tests(build, tests, root, log):
    subprocess.run(['cmake', '--build', str(build), '--parallel', '1', '--verbose', '--target', *tests], cwd=root, stdout=log, stderr=subprocess.STDOUT, check=True)


def run_tests(build, regex, jobs, root, log, keep=()):
    # (-V prefixes each line with its test's number, so the tests run side by side are still read one by one)
    return subprocess.run(['ctest', '--test-dir', str(build), '--parallel', str(jobs), '-R', regex, '-V', '--no-tests=error', '--output-on-failure'], cwd=root, stdout=log, stderr=subprocess.STDOUT, pass_fds=keep).returncode


def earlier_record(out, old_raw, state, build):
    """--resume: 前回の記録から続けてよいかを調べる。返すのは (前回の記録, None) か (None, 続けない理由)。"""
    if old_raw is None:
        return None, '前回の記録がない'
    try:
        old = json.loads(old_raw)
    except ValueError:
        return None, '前回の記録が読めない'
    if not isinstance(old, dict) or not isinstance(old.get('attempts', []), list) or not all(isinstance(a, dict) for a in old.get('attempts', [])) \
            or not isinstance(old.get('failures', []), list) or not isinstance(old.get('audit', {}), dict):
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
    if 'log' not in old:
        return None, '前回の記録にログ名がない（続きからに対応する前の道具の記録）'
    listed = old.get('attempt_logs', [old['log']] if old['log'] else [])
    if not isinstance(listed, list) or any(not isinstance(n, str) or not ATTEMPT_LOG.fullmatch(n) for n in listed) or len(set(listed)) != len(listed):
        return None, '前回の記録のログ名が不正'
    names = list(listed)
    # (the attempt's own log may be missing: the machine died, or the disk was full, before it could be opened)
    # (nor does a log in which no test had started carry anything: ctest was stopped, or could not start, at once)
    if names and names[-1] == old['log'] and (not (out / names[-1]).is_file() or not re.search(rb'Start\s+\d+:', (out / names[-1]).read_bytes())):
        names.pop()
    if any(not (out / n).is_file() for n in names):
        return None, '前回のCTestログがない'
    # 記録に残っているログの指紋と合わないログ、このビルドのものでないログからは続けない。
    known = {a.get('log'): a.get('log_sha256') for a in old.get('attempts', []) if a.get('log')}
    if old.get('status') == 'finished':
        hashes = old.get('audit', {}).get('attempt_log_sha256')
        if isinstance(hashes, list) and len(hashes) == len(listed):
            known.update(zip(listed, hashes))
        elif old['log'] and old.get('audit', {}).get('log_sha256'):
            known[old['log']] = old['audit']['log_sha256']
    project = b'Test project ' + str(build).encode('utf-8')
    for name in names:
        raw = (out / name).read_bytes()
        if name in known and hashlib.sha256(raw).hexdigest() != known[name]:
            return None, '前回のCTestログが記録と違う: ' + name
        if project + b'\n' not in raw and project + b'\r\n' not in raw:
            return None, '前回のCTestログがこのビルドのものでない: ' + name
    old['attempt_logs'] = names
    return old, None


def attempt_summary(old, raw, preset):
    verdicts = outcomes(raw, preset) if raw is not None else {}
    return {'started_at': old.get('started_at'), 'finished_at': old.get('finished_at'),
            'ended': 'finished' if old.get('status') == 'finished' else 'interrupted',
            'ctest_parallel': old.get('ctest_parallel'), 'ctest_exit': old.get('ctest_exit'), 'cli23': old.get('cli23'),
            'environment': old.get('environment'), 'log': old.get('log') if raw is not None else None,
            'log_sha256': hashlib.sha256(raw).hexdigest() if raw is not None else None,
            'passed': sum(1 for v in verdicts.values() if v[0] == 'passed'),
            'failed': {name: v[1] for name, v in sorted(verdicts.items()) if v[0] == 'failed'},
            'unfinished': sorted(name for name, v in verdicts.items() if v[0] == 'unfinished'),
            'failures': old.get('failures', [])}


def run(args, root=None):
    out = Path(args.output).resolve()
    out.mkdir(parents=True, exist_ok=True)
    try:
        lock = hold(out)
    except RuntimeError as error:
        print(json.dumps({'preset': args.preset, 'passed': False, 'failures': [str(error)]}, ensure_ascii=False))
        return 1
    try:
        return run_held(args, root, out, (lock,) if lock is not None else ())
    finally:
        if lock is not None:
            os.close(lock)


def run_held(args, root, out, keep):
    root = Path(root).resolve() if root else Path(__file__).resolve().parents[2]
    plan = json.loads(Path(args.plan).read_text(encoding='utf-8'))
    matches = [row for row in plan['matrix']['include'] if row['preset'] == args.preset]
    if len(matches) != 1 or not matches[0]['tests']:
        raise ValueError('構成の選択が空または曖昧です')
    row = matches[0]
    build = Path(args.build).resolve()
    result_file = out / 'result.json'
    resume = getattr(args, 'resume', False)
    old_raw = result_file.read_bytes() if resume and result_file.is_file() else None
    state = {'preset': args.preset, 'phase': plan['phase'], 'suite': row['suite'], 'selected': row['tests'],
             'excluded_contracts': row['excluded_contracts'], 'started_at': now(), 'status': 'running',
             'source_before': source_pins(root), 'cli23': 'not_run', 'performance': 'not_run', 'plan_hash': plan_hash(plan),
             'ctest_parallel': args.jobs, 'environment': environment(), 'log': 'ctest-output.log'}
    if old_raw is None:
        write_state(result_file, state)  # (with a record to resume from, that record stays until this run knows whether it continues it)
    failures = []
    decided = old_raw is None
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
        commands = binaries(build, built)
        state['binary_before'] = binary_pins(commands)
        state['configuration'] = (build / 'CMakeCache.txt').read_text(encoding='utf-8')
        config_errors = configuration_errors(state['configuration'], args.preset)
        if config_errors:
            raise ValueError('; '.join(config_errors))
        to_run = list(row['tests'])
        earlier_logs = []
        if resume:
            old, reason = earlier_record(out, old_raw, state, build)
            if old is not None and old.get('status') == 'finished' and old.get('passed') is True and not old.get('failures'):
                # 前回がそのまま合格で、計画・ソース・実行物・構成も同じ。記録は書き換えない。
                print(json.dumps({'preset': args.preset, 'count': len(row['tests']), 'passed': True, 'failures': [],
                                  'resume': {'used': True, 'carried_over': len(row['tests']), 'to_run': 0, 'unchanged': True}}, ensure_ascii=False))
                return 0
            carried = {}
            if old is not None:
                earlier_logs = old['attempt_logs']
                raws = [(out / n).read_bytes() for n in earlier_logs]
                broken, carried, _, _ = walk_attempts(raws, row['tests'], args.preset, earlier_logs)
                if broken:
                    old, reason = None, '前回のログが監査に通らない: ' + broken[0]
                elif not carried:
                    old, reason = None, '引き継げる合格がない'
                elif len(carried) == len(row['tests']) and not ran_to_the_end(raws[-1]):
                    old, reason = None, '全部合格しているが、最後の回のCTestが走り切ったと書いていない'
            if old is None:
                earlier_logs = []
                state['resume'] = {'used': False, 'reason': reason}
            else:
                own = old['log'] if old['log'] in earlier_logs else None
                state['attempts'] = list(old.get('attempts', [])) + [attempt_summary(old, (out / own).read_bytes() if own else None, args.preset)]
                to_run = [n for n in row['tests'] if n not in carried]
                state['log'] = f"ctest-output.attempt-{len(state['attempts']) + 1}.log" if to_run else None
                state['attempt_logs'] = earlier_logs + ([state['log']] if to_run else [])
                state['resume'] = {'used': True, 'carried_over': len(carried), 'to_run': len(to_run)}
        decided = True
        # この回が作っていないログを、記録が指すことがないようにする: 引き継がないログは、記録を書く前に消す。
        for stale in list(out.glob('ctest-output*.log')) + ([] if earlier_logs else list(out.glob('cli23*.log'))):
            if stale.name not in earlier_logs:
                stale.unlink()
        qtfiles = {t['name']: Path(t['command'][t['command'].index('-o') + 1].removesuffix(',txt'))
                   for t in built['tests'] if '-o' in t['command']}
        for file in qtfiles.values():
            file.unlink(missing_ok=True)  # 前回ログを再利用しない。
        # (the pins and the configuration are on disk before the tests start: a run that dies in the tests can be
        # continued only against them)
        write_state(result_file, state)
        log_file = out / 'ctest-output.log'
        if to_run:
            log_file = out / state['log']
            with log_file.open('wb') as log:
                state['ctest_exit'] = run_tests(build, '^(' + '|'.join(re.escape(n) for n in to_run) + ')$', args.jobs, root, log, keep=keep)
        else:
            state['ctest_exit'] = 0  # (everything was carried over: ctest is not run)
        qt_logs = None
        if args.preset.startswith('windows'):
            qt_logs = {}
            for name, file in qtfiles.items():
                if file.is_file():
                    data = file.read_bytes(); qt_logs[name] = data
                    (out / file.name).write_bytes(data)
        if earlier_logs:
            state['audit'] = audit_attempts([(out / n).read_bytes() for n in state['attempt_logs']], row['tests'], args.preset, state['attempt_logs'])
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
            if earlier_logs and (out / 'cli23.log').is_file():
                # 前の回のCLI23の記録は、回の番号を付けて残す。
                os.replace(out / 'cli23.log', out / f"cli23.attempt-{len(state['attempts'])}.log")
            with (out / 'cli23.log').open('wb') as log:
                p = subprocess.run([pyref, str(root / 'tools/acceptance/m3a_access_check.py'), str(executable)], cwd=root, stdout=log, stderr=subprocess.STDOUT, pass_fds=keep)
            state['cli23'] = 'pass' if p.returncode == 0 else 'fail'
            if p.returncode:
                failures.append('CLI23失敗')
        state['binary_after'] = binary_pins(commands)
        if state['binary_before'] != state['binary_after']:
            failures.append('試験中に実行物が変化')
    except Exception as error:
        failures.append(str(error))
    if not decided:
        # 続けるかどうかを決める前に止まった（登録・ビルド・構成の失敗）。
        try:
            old = json.loads(old_raw)
            was_passed = isinstance(old, dict) and old.get('status') == 'finished' and old.get('passed') is True
        except ValueError:
            was_passed = False
        if not was_passed:
            # 合格でない前回の記録は、続きから走らせる元なので書き換えずに残す。合格の記録は、この回の不合格で置き換える。
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
        summary['earlier_failures'] = [f for a in state.get('attempts', []) for f in a.get('failures') or []]
    print(json.dumps(summary, ensure_ascii=False))
    return 1 if failures else 0


def pair_results(folder, plan):
    expected = {r['preset']: r for r in plan['matrix']['include']}
    files = list(Path(folder).rglob('result.json'))
    results = [(p, json.loads(p.read_text(encoding='utf-8'))) for p in files]
    rows = {r['preset']: r for _, r in results}
    failures = []
    resumed = {}
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
            if sorted(p.name for p in path.parent.glob('ctest-output*.log')) != sorted(attempt_logs if isinstance(attempt_logs, list) else ['ctest-output.log']):
                raise ValueError('CTestログが記録と合わない（記録にないログがある・足りない）')
            if attempt_logs is not None:
                # 続きから走らせた記録: 各回のログを全部読み直す。
                if not attempt_logs or any(not isinstance(n, str) or not ATTEMPT_LOG.fullmatch(n) for n in attempt_logs) or preset.startswith('windows'):
                    raise ValueError('各回のログ名が不正')
                audit = audit_attempts([(path.parent / n).read_bytes() for n in attempt_logs], wanted['tests'], preset, attempt_logs)
                for attempt in row.get('attempts', []):
                    if attempt.get('log') and (attempt['log'] not in attempt_logs or hashlib.sha256((path.parent / attempt['log']).read_bytes()).hexdigest() != attempt.get('log_sha256')):
                        raise ValueError('各回の記録とログが合わない')
                resumed[preset] = {'attempts': len(row.get('attempts', [])) + 1, 'rerun_after_failure': sorted(audit['rerun_after_failure']),
                                   'earlier_failures': [f for a in row.get('attempts', []) for f in a.get('failures') or []]}
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
    # resumed: 続きから走らせた構成（回の数、落ちてから合格した試験、前の回の不合格の理由）。原因はHANDOFFに書く。
    return {'passed': not failures, 'failures': failures, 'presets': sorted(rows), 'performance': 'not_run', 'resumed': resumed}


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
