#!/usr/bin/env python3
"""validation_policy.py の「ソースの閉包」を、実際のビルドと突き合わせる（手で走らせる。計画を作るのには要らない）。

静的ライブラリのビルド（linux-release）では、試験の実行物に入るのは、そこから参照が届くオブジェクトだけ。
実行物ごとに「このオブジェクトのコードが入っているか」を nm の記号で調べ、入っているオブジェクトのソースが
全部その試験の閉包にあることを確かめる（閉包 ⊇ リンクされたもの）。一つでも漏れがあれば不合格（終了コード 1）。

入っているかの判定: オブジェクト自身が定義する外部の強い記号のうち、ビルドの中でそのオブジェクトだけが定義するもの
（一意な記号）が、実行物の記号表に一つでもあれば「入っている」。一意な記号を持たないオブジェクトは判定できないので
数と名前を出す。入っているオブジェクトの一意な記号が全部はそろっていない場合（strip・--gc-sections・LTO のビルド）も
数を出す。その場合、この突き合わせは当てにならない。

使い方（工程出口で Release のビルドがあるとき）:
  python3 tools/ci/validation_closure_check.py --build build/linux-release
"""
import argparse
import concurrent.futures
import json
from pathlib import Path
import re
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import validation_policy as policy  # noqa: E402

STRONG = set('TDBRSGAC')


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

    def nm_object(item):
        return item, symbols(item)
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

    report = {'build': str(build), 'source_root': str(root), 'targets_in_cmake': len(targets), 'binaries_checked': 0,
              'binaries_missing': sorted(set(targets) - set(places)), 'binaries_unreadable': [], 'objects': len(objects),
              'objects_unreadable': unreadable_objects, 'objects_without_a_unique_symbol': unjudged,
              'partly_present': 0, 'linked': 0, 'in_closure': 0, 'extra': 0, 'misses': [], 'not_in_this_tree': [], 'by_library': {}}
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
        for key, found in sorted(hits.items()):
            if found < unique[key] and made_from[key] == 1:
                report['partly_present'] += 1
            kind, value = key
            if kind == 'code':
                linked.add(value)
                if value not in tree.text:
                    report['not_in_this_tree'].append([name, value])
                    continue
            if kind != 'object' and not covered(kind, value, closure):
                report['misses'].append([name, kind, value])
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
    report['passed'] = not report['misses'] and report['binaries_checked'] > 0 and not report['partly_present']
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
        print(json.dumps({'passed': False, 'error': str(error)}, ensure_ascii=False))
        return 1
    if args.output:
        Path(args.output).write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    shown = dict(report, misses=report['misses'][:50], not_in_this_tree=report['not_in_this_tree'][:20])
    print(json.dumps(shown, ensure_ascii=False, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
