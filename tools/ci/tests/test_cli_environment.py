"""CLI23の作業場所・cgroupをDocker固定にしない回帰試験。"""
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('cli_environment', ROOT / 'tools/acceptance/cli_environment.py')
assert spec and spec.loader
helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)


class CliEnvironmentTests(unittest.TestCase):
    def test_workspace_build_directory_is_used(self):
        with tempfile.TemporaryDirectory() as folder:
            path = helper.fixture_directory(Path(folder), {})
            self.assertEqual(path.parent, Path(folder) / 'build')
            self.assertTrue(path.is_dir())

    def test_explicit_directory_is_respected(self):
        with tempfile.TemporaryDirectory() as folder:
            path = helper.fixture_directory(Path(folder), {'GENKO_TEST_TMPDIR': str(Path(folder) / 'separate')})
            self.assertEqual(path.parent, Path(folder) / 'separate')

    def cg(self, folder, path):
        cg = Path(folder) / path
        cg.mkdir(parents=True, exist_ok=True)
        for name in helper.CGROUP_FILES:
            (cg / name).write_text('unit fixture')
        return cg

    def test_host_process_cgroup_is_used(self):
        with tempfile.TemporaryDirectory() as folder:
            cg = self.cg(folder, 'system.slice/job.service')
            self.assertEqual(helper.find_cgroup(Path(folder), '0::/system.slice/job.service\n'), cg)

    def test_container_root_cgroup_is_used(self):
        with tempfile.TemporaryDirectory() as folder:
            root = self.cg(folder, '')
            self.assertEqual(helper.find_cgroup(root, '0::/not-visible-in-this-namespace\n'), root)

    def test_missing_resource_controls_are_not_invented(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(RuntimeError):
                helper.find_cgroup(Path(folder), '0::/missing\n')

    def v1(self, folder, memory_path, cpu_dir='cpu', cpu_path=''):
        memory = Path(folder) / 'memory' / memory_path
        memory.mkdir(parents=True, exist_ok=True)
        (memory / 'memory.oom_control').write_text('oom_kill_disable 0\nunder_oom 0\noom_kill 3\n')
        (memory / 'memory.failcnt').write_text('2\n')
        (memory / 'memory.limit_in_bytes').write_text('2147483648\n')
        (memory / 'memory.max_usage_in_bytes').write_text('1048576\n')
        cpu = Path(folder) / cpu_dir / cpu_path
        cpu.mkdir(parents=True, exist_ok=True)
        (cpu / 'cpu.cfs_quota_us').write_text('-1\n')
        (cpu / 'cpu.cfs_period_us').write_text('100000\n')
        return memory, cpu

    def test_v2_records_are_read_as_they_are(self):
        with tempfile.TemporaryDirectory() as folder:
            cg = Path(folder) / 'job'
            cg.mkdir()
            (cg / 'memory.events').write_text('low 0\nhigh 0\nmax 0\noom 1\noom_kill 0\n')
            (cg / 'memory.max').write_text('2147483648\n')
            (cg / 'memory.peak').write_text('4096\n')
            (cg / 'cpu.max').write_text('100000 100000\n')
            found = helper.find_resources(Path(folder), '0::/job\n')
            self.assertEqual(found.events(), {'low': 0, 'high': 0, 'max': 0, 'oom': 1, 'oom_kill': 0})
            self.assertEqual((found.memory_max(), found.memory_peak(), found.cpu_max()), ('2147483648', '4096', '100000 100000'))

    def test_v1_controllers_records_are_used(self):
        with tempfile.TemporaryDirectory() as folder:
            self.v1(folder, 'process/job')
            found = helper.find_resources(Path(folder), '4:memory:/process/job\n1:cpu:/\n0::/\n')
            self.assertEqual(found.events(), {'max': 2, 'oom': 3, 'oom_kill': 3})
            self.assertEqual((found.memory_max(), found.memory_peak(), found.cpu_max()), ('2147483648', '1048576', 'max 100000'))

    def test_v1_joined_cpu_controllers(self):
        with tempfile.TemporaryDirectory() as folder:
            self.v1(folder, 'job', 'cpu,cpuacct', 'job')
            found = helper.find_resources(Path(folder), '5:memory:/job\n3:cpu,cpuacct:/job\n')
            self.assertEqual(found.cpu_max(), 'max 100000')

    def test_v1_without_a_controller_is_not_invented(self):
        with tempfile.TemporaryDirectory() as folder:
            memory, cpu = self.v1(folder, 'job')
            (memory / 'memory.oom_control').unlink()
            with self.assertRaises(RuntimeError):
                helper.find_resources(Path(folder), '4:memory:/job\n1:cpu:/\n')


if __name__ == '__main__':
    unittest.main()
