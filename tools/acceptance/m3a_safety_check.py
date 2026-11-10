#!/opt/pyref/bin/python
"""Hermes CLI regressions: terminating fills, acyclic folders, decodable raster inputs."""
import base64
import io
import json
import os
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

sys.path.insert(0, '/src/src')
from PIL import Image
from genko.io import save_episode
from genko.models import PageSpec, new_episode

G = os.environ.get('GENKO_BIN', '/src/build/hermes-m3a-release/src/api/genko')
ROOT = Path(tempfile.mkdtemp(prefix='hermes-m3a-safety-', dir='/src/build'))
os.environ['GENKO_CONFIG_DIR'] = str(ROOT / 'config')


def cli(args, timeout=20):
    return subprocess.run([G, *map(str, args)], capture_output=True, text=True, timeout=timeout)


def apply(book, ops, timeout=20):
    path = ROOT / 'ops.json'
    path.write_text(json.dumps(ops), encoding='utf-8')
    return cli(['apply', book, path, '--agent', 'human:確認'], timeout)


def fresh(name):
    book = ROOT / (name + '.genko')
    result = cli(['new', book, '--pages', '1'])
    assert result.returncode == 0, result.stderr + result.stdout
    result = apply(book, [{'op': 'name_ok', 'page': 1}])
    assert result.returncode == 0, result.stderr + result.stdout
    return book


def fingerprint(book):
    out = {}
    for p in sorted(book.rglob('*')):
        rel = p.relative_to(book).as_posix()
        if p.is_dir():
            out[rel] = None
        elif rel == 'project.lock':
            lock = json.loads(p.read_bytes())
            assert lock.get('released') is True, 'lock remained acquired'
            lock.pop('released_at', None)
            out[rel] = json.dumps(lock, sort_keys=True).encode()
        else:
            out[rel] = p.read_bytes()
    return out


def refuse_unchanged(book, op):
    before = fingerprint(book)
    try:
        result = apply(book, [{'op': 'set_note', 'page': 1, 'note': '漏れてはいけない変更'}, op], timeout=6)
    except subprocess.TimeoutExpired:
        raise AssertionError('operation did not terminate within 6 seconds') from None
    assert result.returncode != 0, 'unsafe input was accepted: ' + result.stdout[:500]
    assert fingerprint(book) == before, 'refusal changed saved book bytes or paths'


def flood():
    book = fresh('flood')
    blob = io.BytesIO()
    Image.new('RGB', (3, 1), 'white').save(blob, format='PNG')
    result = apply(book, [{'op': 'put_raster', 'page': 1, 'layer': 'ink',
                          'png_base64': base64.b64encode(blob.getvalue()).decode()}])
    assert result.returncode == 0, result.stderr + result.stdout
    # Positive control must terminate and paint a distinct valid colour.
    normal = apply(book, [{'op': 'flood_fill', 'page': 1, 'layer': 'ink', 'rgb': [0, 0, 0]}], timeout=6)
    assert normal.returncode == 0, normal.stderr + normal.stdout
    reset = apply(book, [{'op': 'put_raster', 'page': 1, 'layer': 'ink',
                         'png_base64': base64.b64encode(blob.getvalue()).decode()}])
    assert reset.returncode == 0, reset.stderr + reset.stdout
    refuse_unchanged(book, {'op': 'flood_fill', 'page': 1, 'layer': 'ink', 'rgb': [300, 255, 255]})


def group():
    book = fresh('group')
    normal = apply(book, [{'op': 'add_layer', 'page': 1, 'id': 'F', 'kind': 'folder'},
                          {'op': 'add_layer', 'page': 1, 'id': 'C', 'kind': 'pen', 'parent': 'F'}])
    assert normal.returncode == 0, normal.stderr + normal.stdout
    refuse_unchanged(book, {'op': 'group_layers', 'page': 1, 'ids': ['F', 'C'], 'id': 'N'})


def bad_png():
    book = fresh('png')
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    ihdr = struct.pack('>IIBBBBB', 1, 1, 8, 6, 0, 0, 0)
    blob = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', b'not zlib') + chunk(b'IEND', b'')
    refuse_unchanged(book, {'op': 'put_raster', 'page': 1, 'layer': 'ink',
                          'png_base64': base64.b64encode(blob).decode()})


results = []
for name, check in [('flood_fill', flood), ('group_layers', group), ('invalid_png', bad_png)]:
    if len(sys.argv) > 1 and name != sys.argv[1]:
        continue
    try:
        check()
        results.append({'case': name, 'passed': True})
    except AssertionError as error:
        results.append({'case': name, 'passed': False, 'error': str(error)})
print(json.dumps({'checks': results, 'fixture_root': str(ROOT)}, ensure_ascii=False, indent=1))
sys.exit(0 if results and all(x['passed'] for x in results) else 1)
