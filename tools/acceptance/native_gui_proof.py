#!/usr/bin/env python3
"""Run the real Qt xcb window on a disposable X server; retain GUI proof."""
import hashlib
import json
import os
from pathlib import Path
import selectors
import subprocess
import sys

root = Path('/src')
output = root / 'build/m2g6-gui-proof'
output.mkdir(parents=True, exist_ok=True)
binary = root / 'build/linux-debug/tests/test_gui_save'
source_names = [
    'native/src/app/session.cpp', 'native/src/app/session.hpp',
    'native/src/app/main_window.cpp', 'native/src/app/main_window.hpp',
    'native/src/app/main_window_actions.cpp',
    'native/tests/unit/test_session.cpp', 'native/tests/gui/test_gui_save.cpp',
    'tools/acceptance/native_gui_proof.py',
]
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
pins = {name: sha(root / name) for name in source_names}
before = sha(binary)
server = subprocess.Popen(
    ['Xvfb', '-displayfd', '1', '-screen', '0', '1600x1000x24', '-nolisten', 'tcp'],
    stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
try:
    assert server.stdout is not None
    selector = selectors.DefaultSelector()
    selector.register(server.stdout, selectors.EVENT_READ)
    if not selector.select(10):
        raise RuntimeError('Xvfb did not announce its ready display')
    display = server.stdout.readline().strip()
    if not display.isdecimal():
        raise RuntimeError('Invalid display from Xvfb: ' + repr(display))
    env = dict(os.environ, DISPLAY=':' + display, QT_QPA_PLATFORM='xcb',
               GENKO_GUI_PROOF_DIR=str(output))
    result = subprocess.run([str(binary)], env=env, timeout=600)
    photos = ['01-saved', '02-failed-memory-only', '03-close-cancel-default',
              '04-cancel-preserved', '05-reopened-saved']
    unchanged = pins == {name: sha(root / name) for name in source_names}
    artifacts = {name: sha(output / (name + '.png')) for name in photos}
    resources = {}
    for name in ['memory.max', 'memory.peak', 'memory.events', 'cpu.max']:
        path = Path('/sys/fs/cgroup') / name
        resources[name] = path.read_text().strip()
    assert resources['memory.max'] == '2147483648'
    assert resources['cpu.max'] == '100000 100000'
    events = dict(line.split() for line in resources['memory.events'].splitlines())
    assert events['oom'] == '0' and events['oom_kill'] == '0'
    record = {
        'classification': '主担当・未統合木の実Qt xcb画面と保存/終了キャンセル/再読込検証',
        'platform': 'xcb', 'display': ':' + display, 'exit_code': result.returncode,
        'source_pins': pins, 'source_unchanged': unchanged,
        'binary_sha256': before, 'binary_unchanged': before == sha(binary),
        'artifacts': artifacts, 'resources': resources,
    }
    (output / 'native-proof.json').write_text(json.dumps(record, ensure_ascii=False, indent=2) + '\n')
    print(json.dumps(record, ensure_ascii=False))
    if result.returncode != 0 or not unchanged or before != sha(binary):
        sys.exit(1)
finally:
    server.terminate()
    try:
        server.wait(timeout=5)
    except subprocess.TimeoutExpired:
        server.kill()
        server.wait()
