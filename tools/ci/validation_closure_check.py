#!/usr/bin/env python3
"""validation_policy.py の「ソースの閉包」を、実際のビルドと突き合わせる（手で走らせる。計画を作るのには要らない）。

静的ライブラリのビルド（linux-release）では、試験の実行物に入るのは、そこから参照が届くオブジェクトだけ。
実行物ごとに「このオブジェクトのコードが入っているか」を nm の記号で調べ、入っているオブジェクトのソースが
全部その試験の閉包にあることを確かめる（閉包 ⊇ リンクされたもの）。一つでも漏れがあれば不合格（終了コード 1）。

入っているかの判定: オブジェクト自身が定義する外部の強い記号のうち、ビルドの中でそのオブジェクトだけが定義するもの
（一意な記号）が、実行物の記号表に一つでもあれば「入っている」。一意な記号を持たないオブジェクトは判定できないので
数と名前を出す。入っているオブジェクトの一意な記号が全部はそろっていない場合（strip・--gc-sections・LTO のビルド）も
数を出す。その場合、この突き合わせは当てにならない。実行物より新しいオブジェクトがある場合（ビルドの途中・リンクし直す前）は、
その実行物の名前を出して不合格にする（漏れの有無は、リンクし直してから見る）。

ヘッダーは ninja の依存の記録（.ninja_deps）で確かめる: コンパイラが実際に読んだ、木の中のヘッダーが全部、そのソースの
インクルードの閉包にあること。

合格にしない場合: 漏れがある、読めないオブジェクトがある、登録された実行物が無い・読めない、オブジェクトより新しい
ソース・ヘッダーがある、実行物より新しいオブジェクトがある、依存の記録が無い、閉包を作った木がビルドの木と違う。

この道具が見ない規則（NOT_COVERED）: 子プロセスの判定、データ・文書を名指しで読む試験の判定、CMake の変更の読み方、
名前の規則、写真加工の試験の一覧。Release（-O3、静的）のリンクなので、Debug・MSVC・ASan（共有ライブラリ）のリンクが
残す参照は見ていない。

使い方（工程出口で Release のビルドがあるとき）:
  python3 tools/ci/validation_closure_check.py --build build/linux-release
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import os
import re
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import validation_policy as policy  # noqa: E402

STRONG = set('TDBRSGAC')
NOT_COVERED = [
    '子プロセスの判定（どの試験がどの実行物を動かすか）',
    'データ・文書を名指しで読む試験の判定',
    'CMake の変更の読み方（試験の登録・ソースの並び）',
    '名前の規則（render の系統・app）と写真加工の試験の一覧',
    'Debug・MSVC・ASan（共有ライブラリ）のリンクが残す参照（この道具は Release の静的リンクを見る）',
]


def ninja_deps(path):
    """ninja の依存の記録（.ninja_deps、版 3・4）→ {出力: [入力]}。同じ出力の後の記録が前を置き換える。"""
    data = Path(path).read_bytes()
    if not data.startswith(b'# ninjadeps\n') or len(data) < 16:
        raise ValueError('.ninja_deps が読めない')
    version = struct.unpack_from('<i', data, 12)[0]
    if version not in (3, 4):
        raise ValueError(f'.ninja_deps の版 {version} は読めない')
    at = 16
    paths = []
    found = {}
    while at + 4 <= len(data):
        size = struct.unpack_from('<I', data, at)[0]
        at += 4
        deps, size = size >> 31, size & 0x7fffffff
        record = data[at:at + size]
        at += size
        if len(record) < size:
            break  # (the log's tail cut by a build that was stopped)
        if deps:
            head = 12 if version == 4 else 8
            found[struct.unpack_from('<i', record, 0)[0]] = struct.unpack_from('<%di' % ((size - head) // 4), record, head)
        else:
            paths.append(record[:-4].rstrip(b'\0').decode('utf-8', 'replace'))
    return {paths[out]: [paths[i] for i in inputs if i < len(paths)] for out, inputs in found.items() if out < len(paths)}


def symbols(path, defined_only=False):
    """nm -P の (名前, 種類)。読めなければ None（リンクし直している最中の実行物など）。"""
    for _ in range(2):
        try:
            done = subprocess.run(['nm', '-P'] + (['--defined-only'] if defined_only else []) + [str(path)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        except OSError:
            return None
        if done.returncode == 0:
            rows = []
            for line in done.stdout.decode('utf-8', 'replace').splitlines():
                parts = line.split()
                if len(parts) >= 2:
                    rows.append((parts[0], parts[1]))
            return rows
    return None


def cache_values(build):
    text = (build / 'CMakeCache.txt').read_text(encoding='utf-8', errors='replace')
    return dict(re.findall(r'^([A-Za-z0-9_]+):[^=\r\n]+=([^\r\n]*)\r?$', text, re.M))


def check(build, root=None, jobs=4):
    build = Path(build).resolve()
    cache = cache_values(build)
    if cache.get('GENKO_SANITIZE') or cache.get('GENKO_SHARED_LIBRARIES', 'OFF').upper() in ('ON', '1', 'TRUE'):
        raise ValueError('共有ライブラリのビルドでは調べられない（静的ライブラリの linux-release を使う）')
    native = Path(cache['CMAKE_HOME_DIRECTORY'])
    root = Path(root).resolve() if root else native.parent
    commands = json.loads((build / 'compile_commands.json').read_text(encoding='utf-8'))
    if any('-flto' in c.get('command', '') for c in commands):
        raise ValueError('LTO のビルドでは調べられない')
    tree = policy.SourceTree(root)

    # オブジェクト → ソース（木の中のパス、または生成物の種類）
    objects = {}
    for entry in commands:
        objects[str(Path(entry['directory'], entry['output']))] = Path(entry['file'])
    for big in build.glob('src/**/qrc_*.o'):  # (qt_add_big_resources: the object the resource compiler writes itself)
        objects.setdefault(str(big), big)

    def source_of(obj):
        """(種類, 値): ('code', 木のパス) / ('template', 雛形のパス) / ('qrc', 名前) / ('moc', ライブラリ) / ('other', None)"""
        file = objects[obj]
        name = file.name
        try:
            relative = file.resolve().relative_to(native.parent.resolve()).as_posix()
            if relative.startswith(policy.CODE_ROOTS):
                return 'code', relative
        except ValueError:
            pass
        try:
            inside = file.resolve().relative_to(build).as_posix()
        except ValueError:
            return 'other', None
        resource = re.fullmatch(r'qrc_(\w+?)(?:tmp)?\.(?:cpp|o)', name)
        if resource:
            return 'qrc', resource.group(1)
        if name == 'mocs_compilation.cpp':
            library = re.match(r'src/(\w+)/', inside)
            return ('moc', 'native/src/' + library.group(1)) if library else ('other', None)
        template = 'native/' + inside + '.in'
        if inside.startswith('src/') and (root / template).is_file():
            return 'template', template
        return 'other', None

    # 同じソースから作られたオブジェクト（複数の試験が同じソースを持つ場合）は、一つのものとして数える。
    kinds = {obj: source_of(obj) for obj in objects}
    keys = {obj: kinds[obj] if kinds[obj][0] != 'other' else ('object', obj) for obj in objects}

    def modified(path):
        try:
            return Path(path).stat().st_mtime_ns
        except OSError:
            return 0

    def nm_object(item):
        return item, symbols(item)
    made_at = {}
    for obj, key in keys.items():   # (the newest object of each source)
        made_at[key] = max(made_at.get(key, 0), modified(obj))
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        tables = dict(pool.map(nm_object, sorted(objects)))
    unreadable_objects = sorted(o for o, rows in tables.items() if rows is None)
    owners = {}
    for obj, rows in tables.items():
        for name, kind in rows or ():
            if kind in STRONG:
                owners.setdefault(name, set()).add(keys[obj])
    owner = {name: next(iter(found)) for name, found in owners.items() if len(found) == 1}
    unique = {}
    for name, key in owner.items():
        unique[key] = unique.get(key, 0) + 1
    made_from = {}
    for obj, key in keys.items():
        made_from[key] = made_from.get(key, 0) + 1
    unjudged = sorted(str(key[1]) for key in set(keys.values()) if key[0] in ('code', 'template', 'qrc') and key not in unique)
    consumers = {}
    users_of = {}
    resources = tree.resources()

    def covered(kind, value, closure):
        """このオブジェクトが入っていることを、規則が説明できるか。"""
        if kind == 'code':
            return value in closure
        if kind == 'template':
            if value not in consumers:
                consumers[value] = tree.consumers_of_template(value)
            return consumers[value] is None or bool(consumers[value] & closure)  # (None: the rule sends a change of it to every test)
        if kind == 'qrc':
            if value not in users_of:
                users_of[value] = tree.resource_users(value)
            return value not in resources or not users_of[value] or bool(users_of[value] & closure)  # (no user found: broad, as the rule does)
        if kind == 'moc':
            return tree.moc.get(value, set()) <= closure
        return True

    targets = {name: sources for name, sources in tree.tests.items()}
    targets.update(tree.programs)
    places = {}
    for name in targets:
        for candidate in (build / 'tests' / name, build / 'tests' / (name + '.exe'), build / 'src' / 'api' / name):
            if candidate.is_file():
                places[name] = candidate
                break

    def nm_binary(name):
        return name, symbols(places[name], defined_only=True)
    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        binaries = dict(pool.map(nm_binary, sorted(places)))

    # ヘッダー: コンパイラが読んだ木の中のヘッダーは、そのソースのインクルードの閉包にあるか。ソース・ヘッダーはオブジェクトより古いか。
    own_tree = native.parent.resolve()

    def in_tree(path):
        try:
            return Path(os.path.normpath(os.path.join(build, path))).relative_to(own_tree).as_posix()
        except ValueError:
            return None
    try:
        recorded = ninja_deps(build / '.ninja_deps')
    except (OSError, ValueError, struct.error):
        recorded = None
    header_misses = []
    newer = []
    headers_checked = 0
    for obj, (kind, value) in sorted(kinds.items()):
        if kind != 'code':
            continue
        made = modified(obj)
        read = [in_tree(dep) for dep in (recorded or {}).get(os.path.relpath(obj, build), ())]
        if modified(own_tree / value) > made:
            newer.append([value, value])
        if value not in tree.text:
            continue
        reach = None
        for header in sorted(h for h in read if h and h.startswith(policy.CODE_ROOTS) and h != value):
            if modified(own_tree / header) > made:
                newer.append([value, header])
            if reach is None:
                reach = set()
                stack = [value]
                while stack:
                    for found in tree.includes.get(stack.pop(), ()):
                        if found not in reach:
                            reach.add(found)
                            stack.append(found)
            headers_checked += 1
            if header not in reach:
                header_misses.append([value, header])

    report = {'build': str(build), 'source_root': str(root), 'targets_in_cmake': len(targets), 'binaries_checked': 0,
              'binaries_missing': sorted(set(targets) - set(places)), 'binaries_unreadable': [], 'objects': len(objects),
              'objects_unreadable': unreadable_objects, 'objects_without_a_unique_symbol': unjudged,
              'binaries_older_than_their_objects': [], 'binaries_up_to_date': 0, 'misses_in_binaries_up_to_date': 0,
              'partly_present': 0, 'partly_present_examples': [], 'linked': 0, 'in_closure': 0, 'extra': 0, 'misses': [], 'not_in_this_tree': [], 'by_library': {}}
    code_files = sorted(p for p in tree.text if p.startswith('native/src/') and p.endswith(policy.IMPLEMENTATION_SUFFIXES))
    for name in sorted(places):
        rows = binaries[name]
        if rows is None:
            report['binaries_unreadable'].append(name)
            continue
        hits = {}
        for name_, _ in rows:
            key = owner.get(name_)
            if key is not None:
                hits[key] = hits.get(key, 0) + 1
        closure = tree.closure(targets[name])
        own = [('code', source) for source in targets[name] if ('code', source) in unique]
        if not rows or (own and not any(key in hits for key in own)):
            report['binaries_unreadable'].append(name + '（記号が無い: strip されている？）')
            continue
        report['binaries_checked'] += 1
        linked = set()
        linked_at = modified(places[name])
        stale = any(made_at[key] > linked_at for key in hits)
        if stale:
            report['binaries_older_than_their_objects'].append(name)
        else:
            report['binaries_up_to_date'] += 1
        misses_before = len(report['misses'])
        for key, found in sorted(hits.items()):
            if found < unique[key] and made_from[key] == 1 and not stale:
                report['partly_present'] += 1
                if len(report['partly_present_examples']) < 20:
                    report['partly_present_examples'].append([name, str(key[1]), found, unique[key]])
            kind, value = key
            if kind == 'code':
                linked.add(value)
                if value not in tree.text:
                    report['not_in_this_tree'].append([name, value])
                    continue
            if kind != 'object' and not covered(kind, value, closure):
                report['misses'].append([name, kind, value])
        if not stale:
            report['misses_in_binaries_up_to_date'] += len(report['misses']) - misses_before
        product = {p for p in linked if p.startswith('native/src/')}
        inside = {p for p in closure if p.startswith('native/src/') and p.endswith(policy.IMPLEMENTATION_SUFFIXES)}
        report['linked'] += len(product)
        report['in_closure'] += len(inside)
        report['extra'] += len(inside - product)
        for path in code_files:
            library = policy.library_of(path)
            row = report['by_library'].setdefault(library, {'linked': 0, 'in_closure': 0})
            row['linked'] += path in product
            row['in_closure'] += path in inside
    report.update(headers_checked=headers_checked, header_misses=header_misses, sources_newer_than_their_objects=newer,
                  not_covered=NOT_COVERED)
    why = []
    if report['misses']:
        why.append('閉包に無いソースがリンクされている')
    if header_misses:
        why.append('コンパイラが読んだヘッダーがインクルードの閉包に無い')
    if report['objects_unreadable']:
        why.append('読めないオブジェクトがある')
    if report['binaries_missing'] or report['binaries_unreadable'] or not report['binaries_checked']:
        why.append('登録された実行物が無い・読めない')
    if report['partly_present']:
        why.append('一部の記号しか入っていないオブジェクトがある（strip・--gc-sections？）')
    if report['binaries_older_than_their_objects']:
        why.append('実行物より新しいオブジェクトがある（リンクし直す前）')
    if newer:
        why.append('オブジェクトより新しいソース・ヘッダーがある（ビルドし直す前）')
    if recorded is None or not headers_checked:
        why.append('ninja の依存の記録（.ninja_deps）が無い・読めない')
    if root.resolve() != own_tree:
        why.append('閉包を作った木がビルドの木と違う（参考の結果）')
    report['why_not_passed'] = why
    report['passed'] = not why
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--build', required=True, help='静的ライブラリの Release のビルド（compile_commands.json と実行物がある所）')
    parser.add_argument('--source-root', help='閉包を作る木（省くと、ビルドが作られた木）')
    parser.add_argument('--output', help='結果の JSON の置き場')
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    try:
        report = check(args.build, args.source_root, args.jobs)
    except (OSError, ValueError, KeyError) as error:
        print(json.dumps({'passed': False, 'error': str(error), 'not_covered': NOT_COVERED}, ensure_ascii=False))
        return 1
    if args.output:
        Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    older = report['binaries_older_than_their_objects']
    def some(rows, most=10):
        return rows[:most] + ([f'… ほか {len(rows) - most}'] if len(rows) > most else [])
    shown = dict(report, misses=some(report['misses'], 50), not_in_this_tree=some(report['not_in_this_tree'], 20),
                 header_misses=some(report['header_misses'], 50), binaries_older_than_their_objects=some(older),
                 sources_newer_than_their_objects=some(report['sources_newer_than_their_objects']))
    print(json.dumps(shown, ensure_ascii=False, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
