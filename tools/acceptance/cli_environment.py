"""CLI受入の環境だけを解決する。検査内容や資源合格条件は変更しない。"""
from pathlib import Path
import tempfile

CGROUP_FILES = ('memory.events', 'memory.max', 'memory.peak', 'cpu.max')


def fixture_directory(repository, environment):
    parent = Path(environment.get('GENKO_TEST_TMPDIR', str(repository / 'build')))
    parent.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix='hermes-m3a-access-', dir=parent))


def find_cgroup(root=Path('/sys/fs/cgroup'), membership=None):
    if membership is None:
        membership = Path('/proc/self/cgroup').read_text()
    candidates = []
    for line in membership.splitlines():
        parts = line.split(':', 2)
        if len(parts) == 3 and parts[0] == '0' and parts[1] == '':
            relative = Path(parts[2].lstrip('/'))
            # namespace外へ出ず、実際に見えるv2 controllerだけを使う。
            if '..' not in relative.parts:
                candidates.append(root / relative)
    candidates.append(root)
    for candidate in candidates:
        if all((candidate / name).is_file() for name in CGROUP_FILES):
            return candidate
    raise RuntimeError('実cgroup v2資源記録を取得できません。OOM=0やローカル資源合格を作りません')
