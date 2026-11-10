#!/opt/pyref/bin/python
"""Hermes regression: string nonfinite tone gradient angles must be refused atomically."""
import json
import os
import subprocess
import tempfile
from pathlib import Path
from m3_safety_support import book_fingerprint

G = os.environ.get('GENKO_BIN', '/src/build/linux-debug/src/api/genko')
root = Path(tempfile.mkdtemp(prefix='hermes-m3-gradient-'))
os.environ['GENKO_CONFIG_DIR'] = str(root / 'config')


def cli(*args):
    proc = subprocess.run([G, *map(str, args)], capture_output=True, text=True, timeout=120)
    return proc.returncode, json.loads(proc.stdout.strip().splitlines()[-1])


def apply(book, batch):
    ops = root / 'ops.json'
    ops.write_text(json.dumps(batch))
    return cli('apply', book, ops, '--agent', 'human:確認')


book = root / 'book.genko'
assert cli('new', book, '--pages', 1)[0] == 0
assert apply(book, [dict(op='add_tone', page=1, id='valid', gradient=dict(shape='linear', start=0, end=1, angle='30'))])[0] == 0
before = cli('inspect', book, '--full')[1]
saved_before = book_fingerprint(book)
count = 0
for op in ['add_tone', 'set_tone']:
    for angle in ['inf', '-Infinity', 'nan', '1e999']:
        target = 'invalid' if op == 'add_tone' else 'valid'
        code, reply = apply(book, [dict(op='set_note', page=1, note='rollback'),
                                  dict(op=op, page=1, id=target, gradient=dict(shape='linear', start=0, end=1, angle=angle))])
        assert code == 1 and reply.get('ok') is False, (op, angle, 'must refuse', reply)
        assert 'gradient.angle must be a finite number' in reply.get('error', ''), reply
        assert cli('inspect', book, '--full')[1] == before, (op, angle, 'changed on refusal')
        assert book_fingerprint(book) == saved_before, (op, angle, 'saved bytes changed on refusal')
        count += 1
print(json.dumps({'invalid_gradient_angles_refused_unchanged': count, 'finite_string_control': 'pass'}))
