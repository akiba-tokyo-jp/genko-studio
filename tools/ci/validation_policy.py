#!/usr/bin/env python3
"""変更範囲と工程境界から検証計画を作る。未知の範囲は全体へ拡大する。"""
import argparse
import json
from pathlib import Path
import re
import subprocess

PHASES = ('development', 'integration', 'milestone')
SAFETY = {
    'test_json', 'test_model', 'test_storage', 'test_lock', 'test_journal',
    'test_command_bus', 'test_transaction', 'test_session', 'test_save_e2e',
    'test_migrate_e2e', 'test_gui_save', 'test_app_e2e', 'test_cli_e2e',
    'test_gui_layout', 'test_contract_save',
}


def inventory(root):
    text = (Path(root) / 'native/tests/CMakeLists.txt').read_text(encoding='utf-8')
    result = {'normal': set(), 'contract': set(), 'perf': set()}
    for name, body in re.findall(r'genko_test\((test_\w+)\s+(.*?)\)', text, re.S):
        kind = 'perf' if re.search(r'LABELS\s+perf\b', body) else 'contract' if re.search(r'LABELS\s+contract\b', body) else 'normal'
        result[kind].add(name)
    if not result['normal'] or not result['contract']:
        raise ValueError('CTest台帳を抽出できません')
    return result


def make_plan(paths, phase, root):
    if phase not in PHASES:
        raise ValueError('不明な検証段階')
    paths = sorted(set(paths))
    inv = inventory(root)
    all_tests = inv['normal'] | inv['contract']
    related = set(SAFETY)
    reasons = []
    broad = not paths
    infrastructure = False
    native_change = False
    for path in paths:
        if path.startswith('docs/') or path.endswith('.md'):
            continue
        if path in ('.github/workflows/native.yml',) or path.startswith('tools/ci/'):
            infrastructure = True
            reasons.append('CI自身の変更: 実ビルド・非契約全試験・両OSで確認')
        elif path.startswith(('native/cmake/', 'native/vendor/', 'native/third_party/', 'native/packaging/')) or path in ('native/CMakeLists.txt', 'native/CMakePresets.json', 'native/vcpkg.json', 'native/tests/CMakeLists.txt', '.gitattributes'):
            broad = True
            native_change = True
            reasons.append('ビルド・依存・配布・登録の共通境界')
        elif path.startswith(('native/src/core/', 'native/src/storage/', 'native/tests/support/', 'tools/migration/', 'tools/acceptance/', 'src/')):
            broad = True
            native_change = True
            reasons.append('共通モデル・保存・権限・oracle境界')
        elif path.startswith('native/src/render/'):
            native_change = True
            name = Path(path).stem
            related |= {n for n in inv['normal'] if any(k in n for k in ('image', 'png', 'render', 'stroke', 'raster', 'ops', 'pynum', 'pyrandom'))}
            if name in ('raster_ops', 'selection'):
                related |= {n for n in inv['contract'] if 'raster' in n or n in ('test_contract_drawn_by_ops', 'test_contract_save')}
            elif any(k in name for k in ('tone', 'effect', 'ruler')):
                related |= {n for n in all_tests if any(k in n for k in ('tone', 'ruler', 'm3_mixed'))}
            elif any(k in name for k in ('3d', 'mannequin')):
                related |= {n for n in all_tests if '3d' in n or 'm3_mixed' in n}
            else:
                broad = True
                reasons.append('共有描画経路')
        elif path.startswith('native/src/app/'):
            native_change = True
            related |= {n for n in all_tests if n.startswith('test_gui_') or n in ('test_session', 'test_ops_e2e', 'test_contract_save')}
        elif path.startswith('native/tests/'):
            native_change = True
            name = Path(path).stem
            if name in all_tests:
                related.add(name)
            elif name in inv['perf']:
                reasons.append('性能試験変更: 別の性能gateを必須')
            else:
                broad = True
        elif path.startswith('native/'):
            broad = True
            native_change = True
            reasons.append('未分類native変更を安全側で全体へ拡大')
        elif path not in ('.github/workflows/ci.yml', 'pyproject.toml', 'uv.lock') and not path.startswith('tests/'):
            broad = True
            reasons.append('未分類変更')
    if phase == 'milestone':
        broad = True
        native_change = True
        reasons.append('工程出口: 全通常試験を全構成で実行')
    rows = []
    if broad or native_change or infrastructure:
        if broad:
            tier = 'full'
            presets = ['linux-release', 'linux-asan', 'windows-debug', 'windows-release']
            if phase == 'milestone':
                presets.insert(0, 'linux-debug')
        elif infrastructure:
            tier = 'infrastructure' if not native_change else 'related'
            presets = ['linux-release', 'linux-asan', 'windows-debug', 'windows-release']
            if native_change and phase == 'development':
                presets.insert(0, 'linux-debug')
        else:
            tier = 'related' if phase == 'development' else 'integration'
            presets = ['linux-debug', 'linux-asan', 'windows-debug'] if phase == 'development' else ['linux-release', 'linux-asan', 'windows-debug', 'windows-release']
        for preset in presets:
            full = broad or (phase == 'integration' and preset == 'linux-release' and native_change)
            selected = set(all_tests if full else (inv['normal'] | (related if native_change else set())) if infrastructure else related)
            # WindowsにPython oracleは従来どおり置かない。未実施を成功と呼ばない。
            excluded = sorted(selected & inv['contract']) if preset.startswith('windows') else []
            if preset.startswith('windows'):
                selected -= inv['contract']
            selected &= all_tests
            if not selected:
                raise ValueError('native試験の選択が空です')
            rows.append({'preset': preset, 'suite': 'full' if full else 'related',
                         'tests': sorted(selected), 'excluded_contracts': excluded})
    else:
        tier = 'documents'
        reasons.append('文書・Python専用変更: nativeビルドなし')
    return {'schema': 1, 'phase': phase, 'tier': tier, 'paths': paths,
            'reasons': sorted(set(reasons)), 'matrix': {'include': rows},
            'has_native': bool(rows), 'performance_separate': True,
            'performance_required': phase == 'milestone' or any('/perf/' in p for p in paths)}


def choose_baseline(runs, branch, is_ancestor):
    for row in runs:
        sha = row.get('head_sha', '')
        if row.get('head_branch') == branch and row.get('event') == 'push' and row.get('status') == 'completed' and row.get('conclusion') == 'success' and re.fullmatch(r'[0-9a-f]{40}', sha) and is_ancestor(sha):
            return row
    return None


def last_successful_push(root, repository, branch, head):
    # 公開リポジトリのmetadataだけ。認証/permission追加なし。取得不能は全体へ。
    import urllib.parse
    import urllib.request
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('repository識別子が不正')
    query = urllib.parse.urlencode({'branch': branch, 'event': 'push', 'status': 'success', 'per_page': 20})
    url = f'https://api.github.com/repos/{repository}/actions/workflows/native.yml/runs?{query}'
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'genko-validation-policy'}), timeout=15) as response:
        data = json.load(response)
    def ancestor(sha):
        return subprocess.run(['git', 'merge-base', '--is-ancestor', sha, head], cwd=root, stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode == 0
    return choose_baseline(data['workflow_runs'], branch, ancestor)


def changed_paths(root, base, head):
    # diff失敗は例外で停止。差分なしはmake_plan側で全体へ拡大する。
    for ref in (base, head):
        subprocess.run(['git', 'rev-parse', '--verify', '--end-of-options', ref + '^{commit}'], cwd=root, check=True, stdout=subprocess.PIPE)
    return subprocess.check_output(['git', 'diff', '--name-only', '-z', base, head, '--'], cwd=root).decode('utf-8').split('\0')[:-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--base', required=True)
    parser.add_argument('--head', default='HEAD')
    parser.add_argument('--phase', choices=PHASES, default='development')
    parser.add_argument('--output', required=True)
    parser.add_argument('--github-output')
    parser.add_argument('--ci-baseline-branch')
    parser.add_argument('--repository')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    base = args.base
    evidence = None
    baseline_error = None
    if args.ci_baseline_branch:
        if not args.repository:
            parser.error('--repositoryが必要です')
        try:
            evidence = last_successful_push(root, args.repository, args.ci_baseline_branch, args.head)
            base = evidence['head_sha'] if evidence else None
        except (OSError, ValueError, KeyError) as error:
            base = None
            baseline_error = str(error)
    plan = make_plan(changed_paths(root, base, args.head) if base else [], args.phase, root)
    plan.update(base=base, head=args.head, baseline_run=evidence.get('html_url') if evidence else None,
                baseline_error=baseline_error)
    if args.ci_baseline_branch and not base:
        plan['reasons'].append('前回合格を照合できないため全体へ拡大')
    Path(args.output).write_text(json.dumps(plan, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    if args.github_output:
        with open(args.github_output, 'a', encoding='utf-8') as out:
            out.write('matrix=' + json.dumps(plan['matrix']) + '\n')
            out.write('has_native=' + str(plan['has_native']).lower() + '\n')
            for platform in ('linux', 'windows'):
                matrix = {'include': [r for r in plan['matrix']['include'] if r['preset'].startswith(platform)]}
                out.write(platform + '_matrix=' + json.dumps(matrix) + '\n')
    print(json.dumps(plan, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
