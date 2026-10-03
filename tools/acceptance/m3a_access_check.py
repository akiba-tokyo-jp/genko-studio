#!/opt/pyref/bin/python
"""Independent CLI acceptance: cycles, image validity, and real edit permissions."""
import base64
import hashlib
import io
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

from PIL import Image, PngImagePlugin

G = os.environ.get('GENKO_BIN', sys.argv[1] if len(sys.argv) > 1 else '/src/build/hermes-m3a-release/src/api/genko')
ROOT = Path(tempfile.mkdtemp(prefix='hermes-m3a-access-', dir='/src/build'))
os.environ['GENKO_CONFIG_DIR'] = str(ROOT / 'config')
HUMAN = 'human:確認'
AI = 'ai:確認'
OTHER = 'human:別の作者'
results = []
CG = Path('/sys/fs/cgroup')


def memory_events():
    return {k: int(v) for k, v in (row.split() for row in (CG / 'memory.events').read_text().splitlines())}


events_before = memory_events()
binary_before = hashlib.sha256(Path(G).read_bytes()).hexdigest()


def png_bytes():
    out = io.BytesIO()
    metadata = PngImagePlugin.PngInfo()
    metadata.add_text('acceptance', '元PNGバイトを保持')
    Image.new('RGB', (3, 3), 'white').save(out, format='PNG', pnginfo=metadata)
    return out.getvalue()


PNG = png_bytes()
B64 = base64.b64encode(PNG).decode()


def apply(book, ops, actor=HUMAN, timeout: float = 20.0):
    path = ROOT / 'ops.json'
    path.write_text(json.dumps(ops, ensure_ascii=False), encoding='utf-8')
    result = subprocess.run([G, 'apply', str(book), str(path), '--agent', actor],
                            capture_output=True, text=True, timeout=timeout)
    assert result.returncode >= 0, f'CLI terminated by signal {-result.returncode}'
    assert not re.search(r'ERROR: (?:AddressSanitizer|LeakSanitizer)|SUMMARY: .*Sanitizer|runtime error:', result.stderr), result.stderr
    try:
        reply = json.loads(result.stdout)
    except ValueError:
        raise AssertionError('CLI did not return a structured result') from None
    return result, reply


def success(book, ops, actor=HUMAN):
    result, reply = apply(book, ops, actor)
    assert result.returncode == 0 and reply.get('ok') is True, reply
    return reply


def fresh(name, approved=True, pages=1):
    book = ROOT / (name + '.genko')
    p = subprocess.run([G, 'new', str(book), '--pages', str(pages)], capture_output=True, text=True, timeout=20)
    assert p.returncode == 0, p.stdout + p.stderr
    if approved:
        success(book, [{'op': 'name_ok', 'page': i} for i in range(1, pages + 1)])
    return book


def fingerprint(book):
    out = {}
    for p in sorted(book.rglob('*')):
        rel = p.relative_to(book).as_posix()
        if p.is_dir():
            out[rel] = None
        elif rel == 'project.lock':
            value = json.loads(p.read_bytes())
            assert value.get('released') is True, 'project lock remained acquired'
            value.pop('released_at', None)
            out[rel] = hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()
        else:
            out[rel] = hashlib.sha256(p.read_bytes()).hexdigest()
    return out


def refuse(book, op, reason, actor=HUMAN, timeout: float = 20.0, prefix=True):
    # Establish the same mutex actor before byte comparison; owner metadata is not a document edit.
    success(book, [], actor)
    before = fingerprint(book)
    batch = ([{'op': 'set_note', 'page': 1, 'note': '拒否時に保存してはいけない'}] if prefix else []) + [op]
    try:
        p, reply = apply(book, batch, actor, timeout)
    except subprocess.TimeoutExpired:
        raise AssertionError('operation did not terminate') from None
    assert p.returncode == 1 and reply.get('ok') is False, f'unsafe input accepted: {reply}'
    assert re.search(reason, str(reply.get('error', '')), re.I), f'wrong refusal: {reply}'
    assert fingerprint(book) == before, 'refusal changed book bytes/paths or retained its lock'


def check(name, callback):
    try:
        callback()
        row = {'case': name, 'passed': True}
    except (AssertionError, subprocess.TimeoutExpired) as error:
        row = {'case': name, 'passed': False, 'error': str(error)[:1000]}
    results.append(row)
    print(json.dumps(row, ensure_ascii=False), flush=True)


def cycles():
    b = fresh('cycles')
    success(b, [{'op': 'add_layer', 'page': 1, 'kind': 'folder', 'id': 'F'},
                {'op': 'add_layer', 'page': 1, 'kind': 'folder', 'id': 'C', 'parent': 'F'},
                {'op': 'add_layer', 'page': 1, 'kind': 'pen', 'id': 'G', 'parent': 'C'},
                {'op': 'add_layer', 'page': 1, 'kind': 'pen', 'id': 'S'}])
    refuse(b, {'op': 'group_layers', 'page': 1, 'ids': ['F', 'C'], 'id': 'N'}, 'parent|cycle|ancestor|descendant|folder')
    refuse(b, {'op': 'group_layers', 'page': 1, 'ids': ['F', 'G'], 'id': 'N'}, 'parent|cycle|ancestor|descendant|folder')
    success(b, [{'op': 'group_layers', 'page': 1, 'ids': ['G', 'S'], 'id': 'N'}])
    # Ancestor+child is valid when the topmost selected root layer keeps the new folder outside that ancestry.
    safe = fresh('group-safe-ancestry')
    success(safe, [{'op': 'add_layer', 'page': 1, 'kind': 'folder', 'id': 'F'},
                   {'op': 'add_layer', 'page': 1, 'kind': 'pen', 'id': 'C', 'parent': 'F'},
                   {'op': 'add_layer', 'page': 1, 'kind': 'pen', 'id': 'S'}])
    got = success(safe, [{'op': 'group_layers', 'page': 1, 'ids': ['F', 'C', 'S'], 'id': 'N'}])
    selected = {row['id']: row for row in got['snapshot']['pages'][0]['layers'] if row['id'] in ('F', 'C', 'S', 'N')}
    assert len(selected) == 4, 'normal grouping dropped a selected layer'
    assert all(selected[key]['parent_id'] == 'N' for key in ('F', 'C', 'S')), 'normal grouping lost parent links'
    assert selected['N'].get('parent_id') is None, 'normal grouping created an ancestry loop'


def bad_pngs():
    b = fresh('pngs')
    success(b, [{'op': 'put_raster', 'page': 1, 'layer': 'ink', 'png_base64': B64}])
    assert any(p.is_file() and p.read_bytes() == PNG for p in b.rglob('*')), 'normal PNG bytes changed'
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    blobs = []
    for compression, interlace in ((0, 0), (1, 0), (0, 2)):
        header = struct.pack('>IIBBBBB', 1, 1, 8, 6, compression, 0, interlace)
        blobs.append(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', b'not zlib') + chunk(b'IEND', b''))
    for data in blobs:
        refuse(b, {'op': 'put_raster', 'page': 1, 'layer': 'ink', 'png_base64': base64.b64encode(data).decode()}, 'png|image|picture|decode')


def flood():
    b = fresh('flood')
    op = {'op': 'put_raster', 'page': 1, 'layer': 'ink', 'png_base64': B64}
    success(b, [op])
    success(b, [{'op': 'flood_fill', 'page': 1, 'layer': 'ink', 'rgb': [0, 0, 0]}])
    size = Image.open(io.BytesIO(PNG)).size
    black = bytes(size[0] * size[1] * 3)
    pictures = [p.read_bytes() for p in b.rglob('*') if p.is_file()]
    assert any(Image.open(io.BytesIO(data)).convert('RGB').tobytes() == black
               for data in pictures if data.startswith(b'\x89PNG\r\n\x1a\n')), 'normal flood pixels did not change to black'
    success(b, [op])
    # A short bound keeps the original nonterminating 9-pixel loop from consuming the container.
    refuse(b, {'op': 'flood_fill', 'page': 1, 'layer': 'ink', 'rgb': [300, 255, 255]}, 'rgb|color|colour',
           timeout=float(os.environ.get('M3A_FLOOD_DEADLINE', '1')))
    refuse(b, {'op': 'flood_fill', 'page': 1, 'layer': 'ink', 'rgb': [255, 255, -1]}, 'rgb|color|colour')


def layer_lock(name, op, role=False):
    b = fresh('lock-' + name)
    if not role:
        success(b, [{'op': 'add_layer', 'page': 1, 'kind': 'paint', 'id': 'P'}])
    target = {'layer': 'ink'} if role else {'id': 'P'}
    success(b, [{'op': 'put_raster', 'page': 1, **target, 'png_base64': B64}])
    success(b, [op])
    success(b, [{'op': 'set_layer', 'page': 1, **target, 'locked': True}])
    refuse(b, op, 'locked')
    success(b, [{'op': 'set_layer', 'page': 1, **target, 'locked': False}])
    success(b, [op])


def strict(name, op, alternate=False):
    b = fresh('strict-' + name + ('-alternate' if alternate else ''), approved=False)
    success(b, [{'op': 'add_layer', 'page': 1, 'kind': 'paint', 'id': 'P'},
                {'op': 'put_raster', 'page': 1, 'id': 'P', 'png_base64': B64},
                {'op': 'put_raster', 'page': 1, 'layer': 'draft', 'png_base64': B64}])
    draft_id = next(layer['id'] for layer in success(b, [])['snapshot']['pages'][0]['layers'] if layer['role'] == 'draft')
    success(b, [{'op': 'set_meta', 'strict_gates': True}])
    if alternate:
        op = {**op, 'layer_id': draft_id}
    refuse(b, op, 'name_ok|strict', AI)
    draft = {**op}
    draft.pop('layer_id', None)
    if name.startswith('flood'):
        draft['layer'] = 'draft'
    else:
        draft['id'] = draft_id
    success(b, [draft], AI)
    success(b, [{'op': 'name_ok', 'page': 1}])
    success(b, [op], AI)


def paper(value, supplied):
    b = fresh('paper-' + str(len(results)), pages=2)
    op = {'op': 'set_paper', 'rgb': [5, 10, 15]}
    if supplied:
        op['page'] = value
    success(b, [op], AI)
    success(b, [{'op': 'lock_page', 'page': 2}], OTHER)
    refuse(b, {**op, 'rgb': [20, 30, 40]}, 'locked', AI)
    success(b, [{'op': 'unlock_page', 'page': 2}], OTHER)
    success(b, [{'op': 'lock_page', 'page': 2}], AI)
    success(b, [op], AI)


def finite_paste():
    b = fresh('finite-paste')
    success(b, [{'op': 'add_layer', 'page': 1, 'kind': 'pen', 'id': 'P'}])
    for field, value in [('points', [[1, 'nan'], [2, 2]]), ('pressure', [1, 'inf']),
                         ('rotation', [0, 'nan']), ('width_mm', 'inf'), ('opacity', 'nan'),
                         ('pressure_opacity', 'inf')]:
        stroke = {'points': [[1, 1], [2, 2]], field: value}
        refuse(b, {'op': 'paste', 'page': 1, 'layer_id': 'P', 'items': {'strokes': [stroke]}}, 'finite')
    bad_packed = base64.b64encode(struct.pack('<4d', 1, float('inf'), 2, 2)).decode()
    refuse(b, {'op': 'paste', 'page': 1, 'layer_id': 'P', 'items': {'strokes': [{'xy': bad_packed}]}}, 'finite')
    success(b, [{'op': 'paste', 'page': 1, 'layer_id': 'P', 'items': {'strokes': [{'points': [[1, 1], [2, 2]]}]}}])


def nearest_inverse():
    b = fresh('nearest-inverse')
    success(b, [{'op': 'add_layer', 'page': 1, 'kind': 'pen', 'id': 'P'},
                {'op': 'paste', 'page': 1, 'layer_id': 'P', 'items': {'patches': [{'box': [0, 0, 10, 6.667], 'png': B64}]}}])
    op = {'op': 'transform_area', 'page': 1, 'layer_id': 'P', 'area': {'rect': [0, 0, 10, 6.667]}, 'interp': 'nearest'}
    refuse(b, {**op, 'matrix': [1e-20, 0, 0, 1, 0, 0]}, 'inverse|large|finite|flat')
    refuse(b, {**op, 'matrix': [1e-10, 0, 0, 1, 0, 0]}, 'inverse|large|finite')
    success(b, [{**op, 'matrix': [1/60000, 0, -1/60000, 1, 25.4/600, 0]}])
    success(b, [{**op, 'matrix': [0.5, 0, 0, 0.5, 0, 0]}])


def live_masks():
    b = fresh('live-masks')
    # Synthetic page only: enlarge its page spec to exercise the same 200dpi area oracle.
    project = b / 'project.json'
    data = json.loads(project.read_text())
    # Compound-area resolution uses selection's 200dpi, not fill's later 300dpi.
    data['spec']['width_mm'] = 1000.0
    data['spec']['height_mm'] = 1000.0
    project.write_text(json.dumps(data, ensure_ascii=False))
    area = {'rect': [0, 0, 1, 1]}
    for _ in range(6):
        area = {'union': [{'rect': [0, 0, 1, 1]}, area]}
    refuse(b, {'op': 'fill_area', 'page': 1, 'area': area}, 'too many masks', timeout=30)
    success(b, [{'op': 'fill_area', 'page': 1, 'area': {'rect': [0, 0, 1, 1]}}])


check('finite_paste_and_normal', finite_paste)
check('nearest_inverse_and_normal', nearest_inverse)
check('live_masks_and_normal', live_masks)
check('folder_cycles_and_normal_group', cycles)
check('invalid_pngs_and_original_bytes', bad_pngs)
check('flood_termination_and_normal_colour', flood)
locked_ops = [
    ('put_raster', {'op': 'put_raster', 'page': 1, 'id': 'P', 'png_base64': B64}, False),
    ('filter_raster', {'op': 'filter_raster', 'page': 1, 'id': 'P', 'kind': 'invert'}, False),
    ('set_layer_mask', {'op': 'set_layer_mask', 'page': 1, 'id': 'P', 'fill': 'hide'}, False),
    ('paint_mask', {'op': 'paint_mask', 'page': 1, 'id': 'P', 'points': [[2, 2], [3, 3]], 'show': False}, False),
    ('flood_fill', {'op': 'flood_fill', 'page': 1, 'layer': 'ink', 'rgb': [0, 0, 0]}, True)]
for name, op, role in locked_ops:
    check('locked_' + name, lambda name=name, op=op, role=role: layer_lock(name, op, role))
for name, op in [('put', {'op': 'put_raster', 'page': 1, 'id': 'P', 'png_base64': B64}),
                 ('filter', {'op': 'filter_raster', 'page': 1, 'id': 'P', 'kind': 'invert'}),
                 ('flood', {'op': 'flood_fill', 'page': 1, 'layer': 'ink', 'rgb': [0, 0, 0]})]:
    for alternate in (False, True):
        check('strict_' + name + ('_misleading_layer_id' if alternate else ''),
              lambda name=name, op=op, alternate=alternate: strict(name, op, alternate))
for supplied, value in ((False, None), (True, None), (True, ''), (True, 0), (True, False), (True, 0.0)):
    check('all_page_lock_' + ('missing' if not supplied else repr(value)), lambda value=value, supplied=supplied: paper(value, supplied))
events_after = memory_events()
binary_after = hashlib.sha256(Path(G).read_bytes()).hexdigest()
oom_delta = events_after['oom'] - events_before['oom']
oom_kill_delta = events_after['oom_kill'] - events_before['oom_kill']
summary = {'checks': results, 'count': len(results), 'passed': sum(row['passed'] for row in results), 'fixture_root': str(ROOT),
           'binary_sha256': binary_before, 'binary_unchanged': binary_before == binary_after,
           'memory_max': (CG / 'memory.max').read_text().strip(), 'memory_peak': (CG / 'memory.peak').read_text().strip(),
           'cpu_max': (CG / 'cpu.max').read_text().strip(), 'oom_delta': oom_delta, 'oom_kill_delta': oom_kill_delta}
print(json.dumps(summary, ensure_ascii=False, indent=1))
raise SystemExit(0 if len(results) == 23 and all(row['passed'] for row in results)
                 and oom_delta == 0 and oom_kill_delta == 0 and binary_before == binary_after else 1)
