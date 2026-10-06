#!/usr/bin/env python3
"""使い捨てgdev内のGUI開発試験。source・構成・cgroup・実exitを結合する。"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import time

root = Path('/src')
build = root / 'build/photocraft-v1'
result_path = build / 'gui-development-result.json'

def pins():
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted((root / 'native').rglob('*')) if p.is_file()}

def cgroup():
    return {p: (Path('/sys/fs/cgroup') / p).read_text().strip()
            for p in ('cpu.max', 'memory.max', 'memory.events', 'memory.peak')}

result = {'status': 'running', 'scope': 'GUI開発V1のみ', 'source_before': pins(),
          'cgroup_before': cgroup(), 'commands': []}

def save():
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2)+'\n')

save()
try:
    assert result['cgroup_before']['cpu.max'].split() == ['100000', '100000']
    assert result['cgroup_before']['memory.max'] == '2147483648'
    commands = [
        ('gui-build.log', ['cmake', '--build', str(build), '--target', 'test_gui_color', '-j1']),
        ('gui-execution.log', ['xvfb-run', '-a', str(build / 'tests/test_gui_color'),
                             '-o', str(build / 'green-gui.txt')+',txt']),
    ]
    for name, args in commands:
        began = time.monotonic()
        with (build / name).open('wb') as log:
            env = dict(os.environ, QT_QPA_PLATFORM='xcb')
            run = subprocess.run(args, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=1800)
        result['commands'].append({'command': args, 'log': name, 'exit': run.returncode,
                                   'elapsed': time.monotonic()-began})
        save()
        if run.returncode:
            raise RuntimeError(f'{name}: exit {run.returncode}')
    result['status'] = 'passed'
except BaseException as error:
    result['status'] = 'failed'
    result['error'] = str(error)
finally:
    result['source_after'] = pins()
    result['cgroup_after'] = cgroup()
    if result['source_before'] != result['source_after']:
        result['status'] = 'failed'
        result['error'] = '試験中にnative sourceが変更された'
    before = dict(x.split() for x in result['cgroup_before']['memory.events'].splitlines())
    after = dict(x.split() for x in result['cgroup_after']['memory.events'].splitlines())
    result['oom_kill_delta'] = int(after['oom_kill']) - int(before['oom_kill'])
    if result['oom_kill_delta']:
        result['status'] = 'failed'
    exe = build / 'tests/test_gui_color'
    if exe.exists():
        result['binary_sha256'] = hashlib.sha256(exe.read_bytes()).hexdigest()
    save()
print(json.dumps({k: result.get(k) for k in ('status', 'error', 'oom_kill_delta', 'binary_sha256')}, ensure_ascii=False), flush=True)
raise SystemExit(0 if result['status'] == 'passed' else 1)
