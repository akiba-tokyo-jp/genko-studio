#!/opt/pyref/bin/python
"""Run the high-DPI contract without mistaking a QtTest skip for exercised rendering."""
import importlib.util
import json
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET
from pathlib import Path

root = Path(os.environ.get('RENDER_MEMORY_OUTPUT', '/src/build/memory-render-check'))
root.mkdir(parents=True, exist_ok=True)
cgroup = Path('/sys/fs/cgroup')


def events():
    return {k: int(v) for k, v in (line.split() for line in (cgroup / 'memory.events').read_text().splitlines())}


spec = importlib.util.spec_from_file_location('render_harness', '/src/tools/migration/render_harness.py')
assert spec is not None and spec.loader is not None
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
worker_count = module.effective_render_workers(4)
reference = Path(os.environ.get('GENKO_PYREF', '/opt/pyref/bin/python'))
reference_ok = reference.is_file() and os.access(reference, os.X_OK)
xml = root / 'ctest.xml'
xml.unlink(missing_ok=True)
(root / 'result.json').unlink(missing_ok=True)
before = events()
start = time.monotonic()
with (root / 'ctest.log').open('w') as log:
    result = subprocess.run(['ctest', '--test-dir', os.environ.get('RENDER_MEMORY_BUILD', 'build/hermes-m3a-release'), '--output-on-failure',
                             '--no-tests=error', '-R', '^test_contract_render$', '-j1',
                             '--output-junit', str(xml)], stdout=log, stderr=subprocess.STDOUT, timeout=7000)
after = events()
counts = {}
output = ''
if xml.exists():
    suite = ET.parse(xml).getroot()
    counts = {key: suite.get(key) for key in ('tests', 'failures', 'disabled', 'skipped')}
    output = '\n'.join(node.text or '' for node in suite.findall('.//system-out'))
required = ('initTestCase', 'pages', 'others', 'added_line', 'regions', 'cleanupTestCase')
slots = {name: bool(re.search(r'PASS\s+:\s+TestContractRender::' + name + r'\(\)', output)) for name in required}
qt_skipped = bool(re.search(r'SKIP\s+:', output))
report = {'exit_code': result.returncode, 'seconds': round(time.monotonic()-start, 2),
          'requested_workers': 4, 'effective_workers_at_start': worker_count,
          'reference_python': str(reference), 'reference_python_ok': reference_ok,
          'memory_limit_bytes': int((cgroup / 'memory.max').read_text()),
          'memory_peak_bytes': int((cgroup / 'memory.peak').read_text()),
          'memory_events_before': before, 'memory_events_after': after,
          'oom_delta': after['oom']-before['oom'], 'oom_kill_delta': after['oom_kill']-before['oom_kill'],
          'junit': counts, 'qt_slots_passed': slots, 'qt_skipped': qt_skipped,
          'cpu_quota': (cgroup / 'cpu.max').read_text().strip(), 'log': str(root / 'ctest.log')}
report['accepted'] = (result.returncode == 0 and reference_ok and report['oom_delta'] == 0
                      and report['oom_kill_delta'] == 0 and counts.get('tests') == '1'
                      and all(counts.get(key) == '0' for key in ('failures', 'disabled', 'skipped'))
                      and all(slots.values()) and not qt_skipped)
(root / 'result.json').write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n')
print(json.dumps(report, ensure_ascii=False, indent=2), flush=True)
sys.exit(0 if report['accepted'] else 1)
