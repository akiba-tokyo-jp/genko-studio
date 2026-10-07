"""Regression tests for reference rendering within container resource limits."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from unittest import mock

SPEC = importlib.util.spec_from_file_location('render_harness', Path(__file__).with_name('render_harness.py'))
assert SPEC is not None and SPEC.loader is not None
HARNESS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HARNESS)


class RenderResources(unittest.TestCase):
    def test_resource_boundaries(self):
        gib = 1024**3
        cases = [
            ('one_cpu', '100000 100000', 2*gib, 0, 8, 4, 1),
            ('memory_cap', '800000 100000', 2*gib, 0, 8, 4, 1),
            ('two_renderers', '400000 100000', 6*gib, 0, 8, 4, 2),
            ('busy_container', '400000 100000', 4*gib, 3*gib, 8, 4, 1),
            ('requested_ceiling', '800000 100000', 8*gib, 0, 8, 2, 2),
            ('affinity_ceiling', '800000 100000', 8*gib, 0, 1, 4, 1),
            ('fractional_cpu', '50000 100000', 4*gib, 0, 8, 4, 1),
            ('unlimited_cpu', 'max 100000', 4*gib, 0, 8, 4, 1),
            ('unlimited_memory', '800000 100000', 'max', 0, 8, 4, 1),
            ('invalid_memory', '800000 100000', 'invalid', 0, 8, 4, 1),
            ('zero_requested', '400000 100000', 4*gib, 0, 8, 0, 1),
        ]
        for name, cpu, limit, current, affinity, requested, expected in cases:
            with self.subTest(name=name):
                values = {'cpu.max': cpu, 'memory.max': str(limit), 'memory.current': str(current)}
                with mock.patch.object(Path, 'read_text', lambda p: values[p.name]), \
                     mock.patch('os.cpu_count', return_value=8), \
                     mock.patch('os.sched_getaffinity', return_value=set(range(affinity)), create=True):
                    self.assertEqual(HARNESS.effective_render_workers(requested), expected)
        with mock.patch.object(Path, 'read_text', side_effect=FileNotFoundError), \
             mock.patch('os.cpu_count', return_value=8):
            self.assertEqual(HARNESS.effective_render_workers(4), 1)

    def test_skip_mode_is_restored_after_render_failure(self):
        from genko import balloons, render
        originals = [(balloons, 'draw_lines', balloons.draw_lines), (render, '_placed_raster', render._placed_raster)]
        try:
            with tempfile.TemporaryDirectory() as root:
                jobs = Path(root) / 'jobs.json'
                jobs.write_text(json.dumps([{'book': str(Path(root)/'missing.genko'), 'skip_unported': True}]))
                with self.assertRaises(FileNotFoundError):
                    HARNESS.render_jobs(str(jobs))
            for module, name, function in originals:
                self.assertIs(getattr(module, name), function, 'skip mode must not leak to its caller')
        finally:
            for module, name, function in originals:
                setattr(module, name, function)

    def test_normal_skip_normal_pixels_are_isolated(self):
        from PIL import Image
        from genko.io import save_episode
        from genko.models import PageSpec, new_episode
        from genko.ops import apply_ops
        episode = new_episode('参照描画の分離', 1, 1, PageSpec.b5_doujin())
        episode.pages[0].numero = False
        apply_ops(episode, [{'op': 'name_ok', 'page': 1},
                            {'op': 'add_tone', 'page': 1, 'pattern': 'flat', 'density': 1,
                             'area': {'rect': [30, 35, 50, 60]}},
                            {'op': 'add_line', 'page': 1, 'text': '分離の確認', 'x_mm': 60, 'y_mm': 40,
                             'w_mm': 40, 'h_mm': 30}], agent='human:確認')
        with tempfile.TemporaryDirectory() as root:
            book = Path(root) / 'book.genko'
            save_episode(episode, book, actor='human:確認')
            images = []
            for i, skip in enumerate((False, True, False)):
                out = Path(root) / f'image-{i}.png'
                jobs = Path(root) / 'jobs.json'
                jobs.write_text(json.dumps([{'book': str(book), 'page': 1, 'dpi': 24, 'mode': 'proof',
                                            'out': str(out), 'skip_unported': skip}]))
                HARNESS.render_jobs(str(jobs))
                with Image.open(out) as image:
                    images.append(image.convert('RGB').tobytes())
            self.assertNotEqual(images[0], images[1], 'balloon positive control must be visible')
            self.assertEqual(images[0], images[2], 'skip mode must not erase subsequent normal drawing')

    def test_one_cpu_two_gib_does_not_start_four_renderers(self):
        requested = []
        completed = []
        original_read = Path.read_text
        resources = {'cpu.max': '100000 100000', 'memory.max': str(2 * 1024**3),
                     'memory.current': str(200 * 1024**2)}

        def read(path, *args, **kwargs):
            if str(path).startswith('/sys/fs/cgroup/'):
                if path.name not in resources:
                    raise FileNotFoundError(str(path))
                return resources[path.name]
            return original_read(path, *args, **kwargs)

        class Executor:
            def __init__(self, workers, **kwargs):
                requested.append(workers)
            def __enter__(self):
                return self
            def __exit__(self, *args):
                pass
            def map(self, fn, files):
                for path in files:
                    yield fn(path)

        with tempfile.TemporaryDirectory() as root:
            jobs = Path(root) / 'jobs.json'
            original_jobs = [{'book': f'book-{i}', 'out': f'out-{i}-{j}'} for i in range(4) for j in range(2)]
            jobs.write_text(json.dumps(original_jobs))
            with mock.patch.object(Path, 'read_text', read), \
                 mock.patch('os.cpu_count', return_value=8), \
                 mock.patch('os.sched_getaffinity', return_value=set(range(8)), create=True), \
                 mock.patch('concurrent.futures.ProcessPoolExecutor', Executor), \
                 mock.patch.object(HARNESS, 'render_jobs', side_effect=completed.append):
                HARNESS.render_jobs_parallel(str(jobs), 4)
            self.assertEqual(len(completed), 4, 'resource limiting must not discard jobs')
            self.assertEqual(requested, [], 'serial rendering must not create an executor')
            actual_jobs = [job for part in completed for job in json.loads(Path(part).read_text())]
            self.assertEqual(sorted(actual_jobs, key=lambda j: j['out']),
                             sorted(original_jobs, key=lambda j: j['out']), 'every job must survive partitioning')


if __name__ == '__main__':
    unittest.main()
