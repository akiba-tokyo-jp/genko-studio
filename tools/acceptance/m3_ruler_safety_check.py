#!/opt/pyref/bin/python
"""Hermes regressions: reject overflowing ruler sampling and newly nonfinite stroke coordinates."""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path
from m3_safety_support import book_fingerprint

G = os.environ.get('GENKO_BIN', '/src/build/linux-debug/src/api/genko')
root = Path(tempfile.mkdtemp(prefix='hermes-m3-ruler-'))
os.environ['GENKO_CONFIG_DIR'] = str(root / 'config')


def cli(*args):
    proc = subprocess.run([G, *map(str, args)], capture_output=True, text=True, timeout=120)
    assert proc.stdout.strip(), (proc.returncode, proc.stderr)
    return proc.returncode, json.loads(proc.stdout.strip().splitlines()[-1])


def apply(book, batch):
    ops = root / 'ops.json'
    ops.write_text(json.dumps(batch))
    return cli('apply', book, ops, '--agent', 'human:確認')


mode = sys.argv[1] if len(sys.argv) > 1 else 'all'
count = 0
if mode in ('count', 'all'):
    for kind in ['line', 'curve', 'parallel_curve', 'multi_curve', 'radial_curve']:
        book = root / f'{kind}.genko'
        assert cli('new', book, '--pages', 1)[0] == 0
        extra = {'points2': [[0, 1], [1, 1]]} if kind == 'multi_curve' else {'center': [0, -1]} if kind == 'radial_curve' else {}
        assert apply(book, [dict(op='add_ruler', page=1, id='r', kind=kind, points=[[0, 0], [1, 0]], **extra)])[0] == 0
        assert apply(book, [dict(op='add_stroke', page=1, layer='name', ruler_id='r', points=[[0, 0], [50, 0]], stabilize=0, post_smooth=0, taper=False)])[0] == 0
        # Curve rulers' two-point path must be long enough to produce an overflowing arc sample count.
        if kind != 'line':
            extra = {'points2': [[0, 1], [1e19, 1]]} if kind == 'multi_curve' else {}
            assert apply(book, [dict(op='edit_ruler', page=1, id='r', points=[[0, 0], [1e19, 0]], **extra)])[0] == 0
        before = cli('inspect', book, '--full')[1]
        saved_before = book_fingerprint(book)
        code, reply = apply(book, [dict(op='set_note', page=1, note='rollback'),
                                  dict(op='add_stroke', page=1, layer='name', ruler_id='r', points=[[0, 0], [1e19, 0]], stabilize=0, post_smooth=0, taper=False)])
        assert code == 1 and reply.get('ok') is False, (kind, 'must refuse huge sample count', reply)
        assert 'too many points' in reply.get('error', ''), reply
        assert cli('inspect', book, '--full')[1] == before, (kind, 'changed on refusal')
        assert book_fingerprint(book) == saved_before, (kind, 'saved bytes changed on refusal')
        count += 1
if mode in ('coordinates', 'all'):
    book = root / 'coordinates.genko'
    assert cli('new', book, '--pages', 1)[0] == 0
    assert apply(book, [dict(op='add_ruler', page=1, id='r', kind='rect', points=[[10, 10], [20, 20]]),
                        dict(op='ruler_to_layer', page=1, id='r', layer='ink')])[0] == 0
    assert apply(book, [dict(op='edit_ruler', page=1, id='r', points=[[1e308, 0], [1.1e308, 1]])])[0] == 0
    before = cli('inspect', book, '--full')[1]
    saved_before = book_fingerprint(book)
    code, reply = apply(book, [dict(op='set_note', page=1, note='rollback'),
                              dict(op='ruler_to_layer', page=1, id='r', layer='ink')])
    assert code == 1 and reply.get('ok') is False, ('must refuse computed nonfinite coordinates', reply)
    assert 'points must be finite' in reply.get('error', ''), reply
    assert cli('inspect', book, '--full')[1] == before, 'changed on refusal'
    assert book_fingerprint(book) == saved_before, 'saved bytes changed on refusal'
    count += 1
print(json.dumps({'mode': mode, 'ruler_refusals_unchanged': count, 'normal_controls': 'pass'}))
