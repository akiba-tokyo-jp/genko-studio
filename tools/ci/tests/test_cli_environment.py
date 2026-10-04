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


if __name__ == '__main__':
    unittest.main()
