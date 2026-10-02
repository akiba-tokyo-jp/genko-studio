#!/opt/pyref/bin/python
"""Hermes regression: mannequin-only ops must not change another kind of 3D object."""
import json
import os
import subprocess
import tempfile
from pathlib import Path
from m3_safety_support import book_fingerprint

GENKO = os.environ.get('GENKO_BIN', '/src/build/linux-debug/src/api/genko')
root = Path(tempfile.mkdtemp(prefix='hermes-m3-kind-'))
os.environ['GENKO_CONFIG_DIR'] = str(root / 'config')


def run(*args):
    proc = subprocess.run([GENKO, *map(str, args)], capture_output=True, text=True, timeout=120)
    return proc.returncode, json.loads(proc.stdout.strip().splitlines()[-1])


def apply(book, ops):
    source = root / 'ops.json'
    source.write_text(json.dumps(ops), encoding='utf-8')
    return run('apply', book, source, '--agent', 'human:確認')


def main():
    book = root / 'book.genko'
    code, result = run('new', book, '--pages', '1')
    assert code == 0, result
    code, result = apply(book, [
        {'op': 'add_prim3d', 'page': 1, 'kind': 'box', 'id': 'box'},
        {'op': 'add_figure', 'page': 1, 'id': 'figure'},
        {'op': 'add_head', 'page': 1, 'id': 'head'},
        {'op': 'add_hand', 'page': 1, 'id': 'hand'},
        {'op': 'add_mannequin', 'page': 1, 'id': 'man'},
    ])
    assert code == 0, result
    before = run('inspect', book, '--full')[1]
    saved_before = book_fingerprint(book)
    cases = []
    for target in ['box', 'figure', 'head', 'hand']:
        code, result = apply(book, [
            {'op': 'set_note', 'page': 1, 'note': 'must rollback'},
            {'op': 'pose_mannequin', 'page': 1, 'id': target, 'height_mm': 100},
        ])
        assert code == 1 and result.get('ok') is False, (target, 'must refuse', code, result)
        assert f'3D {target} is not a mannequin' in result.get('error', ''), result
        assert run('inspect', book, '--full')[1] == before, (target, 'changed on refusal')
        saved_after = book_fingerprint(book)
        assert saved_after == saved_before, (target, 'saved bytes changed on refusal',
                                             [p for p in saved_before.keys() | saved_after.keys() if saved_before.get(p) != saved_after.get(p)])
        cases.append(target)
    code, result = apply(book, [{'op': 'pose_mannequin', 'page': 1, 'id': 'man', 'height_mm': 100}])
    assert code == 0 and result.get('ok') is True, ('valid mannequin', result)
    print(json.dumps({'refused_unchanged': cases, 'valid_mannequin': 'pass'}, ensure_ascii=False))


if __name__ == '__main__':
    main()
