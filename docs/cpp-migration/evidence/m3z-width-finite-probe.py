#!/usr/bin/env python3
"""Synthetic CLI checks for width/scale rejection, without altering test source."""
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

exe = Path(sys.argv[1])
root = Path(tempfile.mkdtemp(prefix='m3z-finite-', dir='/src/build'))
book = root / 'book.genko'
os.environ['GENKO_CONFIG_DIR'] = str(root / 'config')
binary_before = hashlib.sha256(exe.read_bytes()).hexdigest()

def invoke(args):
    p = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, timeout=30)
    assert not re.search(r'ERROR: (?:AddressSanitizer|LeakSanitizer)|SUMMARY: .*Sanitizer|runtime error:', p.stderr), p.stderr
    assert p.returncode >= 0, p.stderr
    return p, json.loads(p.stdout)

def apply(ops):
    f = root / 'ops.json'
    f.write_text(json.dumps(ops, ensure_ascii=False))
    return invoke(['apply', book, f, '--agent', 'human:確認'])

def fingerprint():
    out = {}
    for f in sorted(book.rglob('*')):
        if f.is_file():
            data = f.read_bytes()
            if f.name == 'project.lock':
                d = json.loads(data)
                assert d['released'] is True
                d.pop('released_at', None)
                data = json.dumps(d, sort_keys=True).encode()
            out[str(f.relative_to(book))] = hashlib.sha256(data).hexdigest()
        else:
            out[str(f.relative_to(book))] = None
    return out

p, reply = invoke(['new', book, '--pages', '1'])
assert p.returncode == 0 and reply['ok']
p, reply = apply([{'op': 'name_ok', 'page': 1},
                  {'op': 'add_layer', 'page': 1, 'id': 'pen-1', 'kind': 'pen'},
                  {'op': 'add_stroke', 'page': 1, 'layer_id': 'pen-1', 'points': [[1,1],[2,2]], 'width_mm': 0.7}])
assert p.returncode == 0 and reply['ok'], reply
p, reply = apply([])
assert p.returncode == 0 and reply['ok'], reply
rows = []
for key in ['width_mm', 'scale']:
    for value in ['nan', 'inf', '-inf', '1e999']:
        before = fingerprint()
        p, reply = apply([{'op': 'set_note', 'page': 1, 'note': '失敗時には残さない'},
                          {'op': 'set_stroke_width', 'page': 1, 'layer_id': 'pen-1',
                           'area': {'poly': [[0, 0], [40, 0], [40, 32], [0, 32]]}, key: value}])
        assert p.returncode == 1 and reply['ok'] is False, reply
        assert 'finite' in str(reply['error']), reply
        assert fingerprint() == before, '拒否で前置変更/ファイル/lockが残った'
        rows.append({'field': key, 'value': value, 'refused': True, 'original_bytes_and_paths_preserved': True})
assert hashlib.sha256(exe.read_bytes()).hexdigest() == binary_before
print(json.dumps({'passed': True, 'count': len(rows), 'checks': rows,
                  'binary_sha256': binary_before, 'sanitizer_checked_per_child': True}, ensure_ascii=False))
