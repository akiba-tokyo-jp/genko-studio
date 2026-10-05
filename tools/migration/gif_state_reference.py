#!/usr/bin/env python3
"""Pillow 12.3.0 oracle for GIF wrapper/core transitions and real mask consumers.

Use --write once to add mixed-state.json and append its MANIFEST entry without
changing any original GIF/table bytes; --check regenerates in memory only.
"""
import argparse
import base64
import hashlib
import io
import json
from pathlib import Path
from types import SimpleNamespace
import PIL
from PIL import Image
from genko.render import _masked, _paint_patch


def pack(image):
    return {'mode': image.mode, 'bands': list(image.getbands()),
            'data': base64.b64encode(image.tobytes()).decode(),
            'rgba': base64.b64encode(image.convert('RGBA').tobytes()).decode()}


def reference(folder):
    raw = (folder / 'colored-global-gray-local.gif').read_bytes()
    result = {'pillow': PIL.__version__, 'input_sha256': hashlib.sha256(raw).hexdigest(), 'cases': []}
    for kind in ['constant', 'image']:
        image = Image.open(io.BytesIO(raw))
        image.load()
        assert image.mode == 'L' and image.im.mode == 'P' and image.readonly == 0
        alpha = 113 if kind == 'constant' else Image.frombytes('L', image.size, bytes((i * 37) % 256 for i in range(image.width * image.height)))
        image.putalpha(alpha)
        result['cases'].append({'kind': kind, **pack(image)})
    loaded = Image.open(io.BytesIO(raw))
    loaded.load()
    result['opened'] = pack(loaded)
    result['explicit_copy'] = pack(loaded.copy())
    result['converted_l'] = pack(loaded.convert('L'))
    rgba = Image.new('RGBA', loaded.size, (31, 61, 91, 255))
    result['layer_mask'] = pack(_masked(SimpleNamespace(mask={'png': raw, 'enabled': True}), rgba))
    canvas = Image.new('RGBA', loaded.size, (0, 0, 0, 0))
    try:
        _paint_patch(canvas, {'png': raw, 'box': [0, 0, loaded.width * 25.4 / 100, loaded.height * 25.4 / 100], 'mode': 'mask', 'rgb': [31, 61, 91]}, 100)
    except Exception as error:
        result['mask_patch'] = {'ok': False, 'type': type(error).__name__, 'message': str(error)}
    else:
        result['mask_patch'] = {'ok': True, **pack(canvas)}
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--write', action='store_true')
    group.add_argument('--check', action='store_true')
    args = parser.parse_args()
    assert PIL.__version__ == '12.3.0'
    root = Path(__file__).resolve().parents[2]
    folder = root / 'native/tests/data/pyref/gif'
    expected = folder / 'mixed-state.json'
    data = reference(folder)
    if args.write:
        assert not expected.exists(), 'Refuse to replace an existing oracle'
        manifest_path = folder.parent / 'MANIFEST.json'
        manifest = json.loads(manifest_path.read_text())
        for name, digest in manifest['files'].items():
            assert hashlib.sha256((folder.parent / name).read_bytes()).hexdigest() == digest, name
        expected.write_text(json.dumps(data, ensure_ascii=False, indent=2) + '\n')
        manifest['files']['gif/mixed-state.json'] = hashlib.sha256(expected.read_bytes()).hexdigest()
        manifest['generators']['gif/mixed-state.json'] = 'PYTHONPATH=src /opt/pyref/bin/python tools/migration/gif_state_reference.py --write (fixed Pillow 12.3.0 inputs)'
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n')
    else:
        assert json.loads(expected.read_text()) == data
    print(json.dumps({'passed': True, 'cases': len(data['cases']), 'mask_patch': {k:v for k,v in data['mask_patch'].items() if k not in ['data','rgba']}, 'oracle_sha256': hashlib.sha256(expected.read_bytes()).hexdigest()}, ensure_ascii=False))


if __name__ == '__main__':
    main()
