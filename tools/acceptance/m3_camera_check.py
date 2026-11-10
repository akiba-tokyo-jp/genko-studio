#!/opt/pyref/bin/python
"""Hermes regression: a legacy prim's own camera must remain alive during render_prims."""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, '/src/src')
os.environ['PYTHONPATH'] = '/src/src'
from genko.io import load_episode, save_episode
from genko.models import PageSpec, new_episode
from genko.render import render_page
from PIL import Image, ImageChops

G = os.environ.get('GENKO_BIN', '/src/build/linux-asan/src/api/genko')
root = Path(tempfile.mkdtemp(prefix='hermes-m3-camera-'))
os.environ['GENKO_CONFIG_DIR'] = str(root / 'config')
ep = new_episode('camera lifetime', 1, 1, PageSpec.b5_doujin())
ep.pages[0].numero = False
ep.pages[0].prims = [dict(id='box', kind='box', pos=[60, 80, 0], size=[20, 20, 20], rot=[0, 0, 0],
                           camera=dict(turn=0.25, tip=0, focal_mm=400, target=[60, 80]))]
legacy = root / 'legacy.genko'
book = root / 'native.genko'
save_episode(ep, legacy, actor='human:確認')
migration = subprocess.run([G, 'migrate', str(legacy), str(book)], capture_output=True, text=True, timeout=120)
assert migration.returncode == 0, (migration.stdout, migration.stderr)
ops = root / 'ops.json'
ops.write_text(json.dumps([dict(op='render_prims', page=1, ids=['box'], surfaces=False, lines=True, layer='ink', kind='gpen')]))
result = subprocess.run([G, 'apply', str(book), str(ops), '--agent', 'human:確認'], capture_output=True, text=True, timeout=120)
assert result.returncode == 0, {'exit': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}
reply = json.loads(result.stdout)
assert reply['ok'] and reply['snapshot']['pages'][0]['ink_stroke_count'] > 0, reply
baseline = subprocess.run([sys.executable, '-m', 'genko', 'apply', str(legacy), str(ops), '--agent', 'human:確認'],
                          capture_output=True, text=True, timeout=120)
assert baseline.returncode == 0, (baseline.stdout, baseline.stderr)
ep = load_episode(legacy)
renders = 0
for mode in ['print', 'proof']:
    png = root / f'{mode}.png'
    native = subprocess.run([G, 'render', str(book), '--page', '1', '--dpi', '72', '--mode', mode, '--out', str(png)],
                            capture_output=True, text=True, timeout=120)
    assert native.returncode == 0, (native.stdout, native.stderr)
    ref = render_page(ep.pages[0], 72, mode=mode, episode=ep).convert('RGB')
    got = Image.open(png).convert('RGB')
    assert got.size == ref.size and ImageChops.difference(got, ref).getbbox() is None, (mode, 'camera pixels differ')
    renders += 1
print(json.dumps({'prim_camera_render_prims': 'pass', 'ink_strokes': reply['snapshot']['pages'][0]['ink_stroke_count'],
                  'camera_renders_equal': renders}))
