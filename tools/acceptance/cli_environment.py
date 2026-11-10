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


# cgroup v1 (a host still on the legacy hierarchy): the memory and cpu controllers' own records.
CGROUP_V1_FILES = {'memory': ('memory.oom_control', 'memory.failcnt', 'memory.limit_in_bytes', 'memory.max_usage_in_bytes'),
                   'cpu': ('cpu.cfs_quota_us', 'cpu.cfs_period_us')}


class Resources:
    """このプロセスの実cgroup記録（v2のファイル、またはv1のmemory/cpu controller自身の記録）。値を作らない。"""

    def __init__(self, v2=None, memory=None, cpu=None):
        self.v2, self.memory, self.cpu = v2, memory, cpu

    def events(self):
        if self.v2 is not None:
            return {k: int(v) for k, v in (row.split() for row in (self.v2 / 'memory.events').read_text().splitlines())}
        control = {k: int(v) for k, v in (row.split() for row in (self.memory / 'memory.oom_control').read_text().splitlines())}
        # v1 keeps no count of OOMs apart from the kills: an OOM kills (oom_kill) or, with the killer disabled, leaves
        # the group under_oom. failcnt is the times the limit was reached (v2's "max", reclaim included), not an OOM.
        return {'max': int((self.memory / 'memory.failcnt').read_text()), 'oom': control['oom_kill'] + control['under_oom'],
                'oom_kill': control['oom_kill']}

    def memory_max(self):
        return (self.v2 / 'memory.max' if self.v2 is not None else self.memory / 'memory.limit_in_bytes').read_text().strip()

    def memory_peak(self):
        return (self.v2 / 'memory.peak' if self.v2 is not None else self.memory / 'memory.max_usage_in_bytes').read_text().strip()

    def cpu_max(self):
        if self.v2 is not None:
            return (self.v2 / 'cpu.max').read_text().strip()
        quota = int((self.cpu / 'cpu.cfs_quota_us').read_text())
        period = (self.cpu / 'cpu.cfs_period_us').read_text().strip()
        return ('max' if quota < 0 else str(quota)) + ' ' + period


def find_resources(root=Path('/sys/fs/cgroup'), membership=None):
    if membership is None:
        membership = Path('/proc/self/cgroup').read_text()
    try:
        return Resources(v2=find_cgroup(root, membership))
    except RuntimeError:
        pass
    found = {}
    for line in membership.splitlines():
        parts = line.split(':', 2)
        if len(parts) != 3 or parts[0] == '0':
            continue
        relative = Path(parts[2].lstrip('/'))
        if '..' in relative.parts:
            continue
        for name in parts[1].split(','):
            if name in CGROUP_V1_FILES:
                directory = root / parts[1] / relative
                if all((directory / f).is_file() for f in CGROUP_V1_FILES[name]):
                    found[name] = directory
    if set(found) == set(CGROUP_V1_FILES):
        return Resources(memory=found['memory'], cpu=found['cpu'])
    raise RuntimeError('実cgroup資源記録（v2、またはv1のmemory/cpu）を取得できません。OOM=0やローカル資源合格を作りません')
