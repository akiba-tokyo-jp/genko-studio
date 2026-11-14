"""検証頻度・選択・安全側への拡大を確認する回帰試験。"""
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('validation_policy', ROOT / 'tools/ci/validation_policy.py')
assert spec is not None and spec.loader is not None
policy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(policy)


class PolicyTests(unittest.TestCase):
    def plan(self, paths, phase='development'):
        return policy.make_plan(paths, phase, ROOT)

    def test_the_cards_way_on_xvfb_only_when_asked(self):
        # (PC5 H-a review P2-2: test_gui_canvas_gpu — GPU と CPU の一致 on Xvfb and Mesa — registered, never in the CI's plan
        # unless asked for, then on Linux with the app's tests and never on Windows)
        inv = policy.inventory(ROOT)
        self.assertIn('test_gui_canvas_gpu', inv['gpu'])
        self.assertNotIn('test_gui_canvas_gpu', inv['normal'] | inv['contract'])
        for phase in policy.PHASES:
            for row in self.plan(['native/src/app/canvas_gpu.cpp'], phase)['matrix']['include']:
                self.assertNotIn('test_gui_canvas_gpu', row['tests'])
        asked = policy.make_plan(['native/src/app/canvas_gpu.cpp'], 'integration', ROOT, gpu=True)
        for row in asked['matrix']['include']:
            self.assertEqual('test_gui_canvas_gpu' in row['tests'], row['preset'].startswith('linux'), row['preset'])

    def test_docs_do_not_start_native_builds(self):
        p = self.plan(['docs/cpp-migration/PLAN.md'], 'integration')
        self.assertEqual(p['matrix']['include'], [])
        self.assertEqual(p['tier'], 'documents')

    def test_the_final_candidate_runs_every_configuration(self):
        # (until 2026-11-14 this was what `milestone` meant; the owner's decisions of 2026-11-13 and 2026-11-14 keep it
        # for the final distribution candidate only: --phase final)
        p = self.plan(['docs/cpp-migration/PLAN.md'], 'final')
        m = {r['preset']: r for r in p['matrix']['include']}
        self.assertEqual(sorted(m), ['linux-asan', 'linux-debug', 'linux-release', 'windows-debug', 'windows-release'])
        inv = policy.inventory(ROOT)
        for name in ('linux-debug', 'linux-release', 'linux-asan'):
            self.assertEqual(m[name]['suite'], 'full')
            self.assertEqual(set(m[name]['tests']), inv['normal'] | inv['contract'])
        for name in ('windows-debug', 'windows-release'):
            self.assertEqual(m[name]['suite'], 'full')
            self.assertEqual(set(m[name]['tests']), inv['normal'])
        self.assertEqual(p['tier'], 'full')
        self.assertTrue(p['performance_required'])
        self.assertTrue(any('最終配布候補' in r for r in p['reasons']))

    def test_a_stage_exit_runs_release_in_full_and_asan_on_what_the_diff_reaches(self):
        # (the owner's decision 2026-11-13: 「変更分と影響箇所だけを見る」; Debug once before the final distribution)
        inv = policy.inventory(ROOT)
        everything = inv['normal'] | inv['contract']
        p = self.plan(['native/src/app/canvas.cpp'], 'milestone')
        m = {r['preset']: r for r in p['matrix']['include']}
        self.assertEqual(sorted(m), ['linux-asan', 'linux-release', 'windows-debug', 'windows-release'])
        self.assertEqual((m['linux-release']['suite'], set(m['linux-release']['tests'])), ('full', everything))
        self.assertEqual(m['linux-asan']['suite'], 'related')
        asan = set(m['linux-asan']['tests'])
        self.assertLess(asan, everything)
        self.assertLessEqual(policy.SAFETY, asan)
        self.assertIn('test_gui_canvas', asan)
        self.assertNotIn('test_fx_lens', asan)
        for name in ('windows-debug', 'windows-release'):
            self.assertEqual(m[name]['suite'], 'related')
            self.assertEqual(set(m[name]['tests']), asan - inv['contract'])
            self.assertEqual(set(m[name]['excluded_contracts']), asan & inv['contract'])
        self.assertEqual(p['tier'], 'milestone')
        self.assertTrue(p['performance_required'])
        # (why each configuration got its scope)
        self.assertTrue(any(r.startswith('linux-release: 全通常試験: 工程出口の安全網') for r in p['reasons']), p['reasons'])
        self.assertTrue(any(r.startswith('linux-asan: 差分に関係する試験') for r in p['reasons']), p['reasons'])
        self.assertTrue(any(r.startswith('工程出口:') for r in p['reasons']))
        # a diff that is sent to every test: ASan and Windows in full too, still without Debug
        for paths in ([], ['native/CMakeLists.txt'], ['native/src/future/new.cpp']):
            wide = {r['preset']: r for r in self.plan(paths, 'milestone')['matrix']['include']}
            self.assertEqual(sorted(wide), ['linux-asan', 'linux-release', 'windows-debug', 'windows-release'])
            self.assertTrue(all(r['suite'] == 'full' for r in wide.values()))
            self.assertEqual(set(wide['linux-asan']['tests']), everything)
        # documents only: the stage exit still has its Release run of everything; ASan keeps the always-run set
        quiet = {r['preset']: r for r in self.plan(['docs/cpp-migration/PLAN.md'], 'milestone')['matrix']['include']}
        self.assertEqual(set(quiet['linux-release']['tests']), everything)
        self.assertEqual(set(quiet['linux-asan']['tests']), policy.SAFETY)

    def test_integration_has_one_full_contract_run(self):
        p = self.plan(['native/src/render/raster_ops.cpp'], 'integration')
        m = {r['preset']: r for r in p['matrix']['include']}
        self.assertEqual(m['linux-release']['suite'], 'full')
        self.assertEqual(m['linux-asan']['suite'], 'related')
        self.assertNotIn('linux-debug', m)
        self.assertNotIn('test_contract_psd', m['linux-asan']['tests'])
        self.assertIn('test_raster_ops', m['linux-asan']['tests'])
        # (raster_ops' own branch: the selection's and the masks' tests, which no other drawing file's change brings)
        for name in ('test_contract_select', 'test_contract_masks', 'test_masks'):
            self.assertIn(name, m['linux-asan']['tests'])
        other = {r['preset']: r for r in self.plan(['native/src/render/tones.cpp'], 'integration')['matrix']['include']}
        for name in ('test_contract_select', 'test_contract_masks', 'test_masks'):
            self.assertNotIn(name, other['linux-asan']['tests'])

    def test_safety_tests_are_registered(self):
        # (every test the plans always take is one CMake registers: a test removed or renamed is not dropped unseen)
        inv = policy.inventory(ROOT)
        self.assertLessEqual(policy.SAFETY, inv['normal'] | inv['contract'])
        self.assertIn('test_refusals_e2e', policy.SAFETY)

    def test_core_change_reaches_every_test_that_holds_it(self):
        # (changed 2026-11-14: "core is everything" was a rule by folder; now every test whose closure holds the file or
        # which runs a program that does. For pynum.cpp that is all but the tests that hold no core code at all.)
        inv = policy.inventory(ROOT)
        everything = inv['normal'] | inv['contract']
        tree = policy.source_tree(ROOT)
        p = self.plan(['native/src/core/pynum.cpp'], 'integration')
        m = {r['preset']: r for r in p['matrix']['include']}
        self.assertEqual(m['linux-release']['suite'], 'full')
        asan = set(m['linux-asan']['tests'])
        holding = {n for n in everything if 'native/src/core/pynum.cpp' in tree.closure(tree.tests[n]) or 'genko' in tree.programs_of()[n]}
        self.assertEqual(asan, holding | policy.SAFETY)
        self.assertGreaterEqual(len(asan), len(everything) - 5)
        for name in ('test_fx_lens', 'test_smart_filters', 'test_precision_merge_controls', 'test_gui_canvas', 'test_contract_psd'):
            self.assertIn(name, asan)
        # a diff of shared code that does reach every test is the whole suite everywhere, with the reason it had
        p = self.plan(['native/src/core/model.hpp', 'native/tests/unit/test_material_preview_cache.cpp'], 'integration')
        self.assertEqual(p['tier'], 'full')
        self.assertTrue(all(r['suite'] == 'full' for r in p['matrix']['include']))
        self.assertIn('共通モデル・保存・権限・oracle境界', p['reasons'])
        self.assertIn('関係する試験が全試験に及ぶ', p['reasons'])

    def test_unknown_native_file_escalates(self):
        p = self.plan(['native/src/future/new.cpp'])
        self.assertEqual(p['tier'], 'full')
        self.assertTrue(all(r['suite'] == 'full' for r in p['matrix']['include']))

    def test_missing_diff_escalates_not_empty(self):
        self.assertEqual(self.plan([])['tier'], 'full')

    def test_mixed_docs_and_source_is_not_documents(self):
        self.assertNotEqual(self.plan(['docs/x.md', 'native/src/app/canvas.cpp'])['tier'], 'documents')

    def test_ci_mixed_with_contract_never_drops_changed_contract(self):
        paths = ['tools/ci/validation_policy.py', 'native/tests/contract/test_contract_psd.cpp']
        for phase in ('development', 'integration'):
            rows = {r['preset']: r for r in policy.make_plan(paths, phase, ROOT)['matrix']['include']}
            for preset, row in rows.items():
                if preset.startswith('linux'):
                    self.assertIn('test_contract_psd', row['tests'])
            self.assertTrue({'linux-release', 'linux-asan', 'windows-debug', 'windows-release'} <= set(rows))
            for row in rows.values():
                self.assertTrue(policy.inventory(ROOT)['normal'] <= set(row['tests']))

    def test_unknown_workflow_and_non_native_files_escalate(self):
        for path in ('.github/future-validation.json', 'tools/future-validation.py'):
            plan = policy.make_plan([path], 'development', ROOT)
            self.assertEqual(plan['tier'], 'full')

    def test_policy_change_runs_real_ci_but_not_all_contracts(self):
        p = self.plan(['.github/workflows/native.yml', 'tools/ci/validation_policy.py'])
        self.assertEqual(len(p['matrix']['include']), 4)
        for r in p['matrix']['include']:
            self.assertIn('test_gui_layout', r['tests'])
            self.assertNotIn('test_contract_psd', r['tests'])

    def test_changed_test_is_never_omitted(self):
        p = self.plan(['native/tests/unit/test_strokes.cpp'])
        self.assertTrue(all('test_strokes' in r['tests'] for r in p['matrix']['include']))

    def test_shared_test_support_reaches_the_tests_that_use_it(self):
        # (changed 2026-11-14: was "tests/support is everything" by folder. testsupport.cpp is in the closure of the
        # tests that include testsupport.hpp, and of no other.)
        inv = policy.inventory(ROOT)
        tree = policy.source_tree(ROOT)
        p = self.plan(['native/tests/support/testsupport.cpp'])
        self.assertEqual(p['tier'], 'related')
        for row in p['matrix']['include']:
            if not row['preset'].startswith('linux'):
                continue
            chosen = set(row['tests'])
            for name in inv['normal'] | inv['contract']:
                uses = 'native/tests/support/testsupport.hpp' in tree.closure(tree.tests[name])
                self.assertEqual(name in chosen, uses or name in policy.SAFETY, name)
            self.assertGreater(len(chosen), 150)
            self.assertIn('test_contract_psd', chosen)
            self.assertNotIn('test_buildinfo', chosen)
        # a file of the support folder no source names: unknown, everything
        self.assertEqual(self.plan(['native/tests/support/agents/story.py'])['tier'], 'full')

    def test_perf_tests_remain_registered_but_separate(self):
        inventory = policy.inventory(ROOT)
        self.assertIn('test_perf_app', inventory['perf'])
        p = self.plan([], 'milestone')
        self.assertFalse(any('test_perf_app' in r['tests'] for r in p['matrix']['include']))
        self.assertIn('performance_separate', p)

    def test_windows_contract_exclusion_is_explicit(self):
        p = self.plan([], 'milestone')
        for r in p['matrix']['include']:
            if r['preset'].startswith('windows'):
                self.assertTrue(r['excluded_contracts'])
                self.assertFalse(any(n.startswith('test_contract') for n in r['tests']))

    def test_invalid_phase_is_rejected(self):
        with self.assertRaises(ValueError):
            self.plan(['native/src/app/canvas.cpp'], 'skip')

    def test_no_duplicate_targets(self):
        p = self.plan(['native/src/render/raster_ops.cpp', 'native/src/app/canvas.cpp'])
        for r in p['matrix']['include']:
            self.assertEqual(r['tests'], sorted(set(r['tests'])))

    def test_tests_run_on_windows_never_skip_for_the_python_oracle(self):
        # Windows runs every test but the contract ones, without a reference Python, and validation_run.py fails a SKIP
        # it does not expect: a comparison with Python belongs in a contract test.
        text = (ROOT / 'native/tests/CMakeLists.txt').read_text(encoding='utf-8')
        normal = policy.inventory(ROOT)['normal']
        offenders = []
        for name, body in re.findall(r'genko_test\((test_\w+)\s+(.*?)\)', text, re.S):
            if name not in normal:
                continue
            sources = re.search(r'SOURCES\s+(.*?)(?:\s(?:DEPS|LABELS|TIMEOUT|GUI)\b|$)', body, re.S)
            for source in sources.group(1).split() if sources else []:
                code = (ROOT / 'native/tests' / source).read_text(encoding='utf-8')
                for statement in re.findall(r'[^;{}]*QSKIP\([^;]*;', code):
                    if re.search(r'python_ref|reference Python|GENKO_PYREF', statement):
                        offenders.append(name + ': ' + ' '.join(statement.split())[:160])
        self.assertTrue(normal)
        self.assertEqual(offenders, [])



SUITE = """
add_executable(genko_lock_helper support/lock_helper.cpp)
add_library(genko_test_support STATIC support/testsupport.cpp)
target_compile_definitions(genko_test_support PRIVATE
  GENKO_CLI="$<TARGET_FILE:genko>"
  GENKO_LOCK_HELPER="$<TARGET_FILE:genko_lock_helper>")
genko_test(test_json SOURCES unit/test_json.cpp LABELS unit)
genko_test(test_alpha SOURCES unit/test_alpha.cpp DEPS genko_core LABELS unit)
genko_test(test_beta SOURCES unit/test_beta.cpp unit/beta_extra.c DEPS genko_core LABELS unit)
genko_test(test_cli SOURCES e2e/test_cli.cpp DEPS genko_test_support LABELS e2e)  # runs genko (a helper)
genko_test(test_contract_gamma SOURCES contract/test_contract_gamma.cpp DEPS genko_test_support LABELS contract TIMEOUT 900)
genko_test(test_perf_delta SOURCES perf/test_perf_delta.cpp DEPS genko_core LABELS perf)
"""

MADE_UP = {
    'native/tests/CMakeLists.txt': SUITE,
    'native/src/api/CMakeLists.txt': 'genko_library(genko_api SOURCES cli.cpp)\nadd_executable(genko main.cpp)\n',
    'native/src/api/main.cpp': '#include "api/cli.hpp"\nint main() { return genko::run(); }\n',
    'native/src/api/cli.hpp': 'namespace genko { int run(); }\n',
    'native/src/api/cli.cpp': '#include "api/cli.hpp"\n#include "core/a.hpp"\nnamespace genko {\nint run() { return alpha(); }\n}\n',
    'native/src/core/CMakeLists.txt': 'genko_library(genko_core\n  SOURCES\n    a.cpp\n    b.cpp\n    window.cpp\n    window_part.cpp\n    elsewhere.cpp\n    lonely.cpp\n)\n',
    'native/src/core/a.hpp': '#include "b.hpp"\nnamespace genko {\nint alpha();\nint moved();\nclass Later;\n}\n',
    'native/src/core/a.cpp': '#include "core/a.hpp"\nnamespace genko {\nint alpha() { return beta(); }\n}\n',
    'native/src/core/b.hpp': 'namespace genko {\nint beta();\nextern const int kTable[];\nbool operator==(const struct Pair&, const struct Pair&);\n}\n',
    'native/src/core/b.cpp': '#include "core/b.hpp"\nnamespace genko {\nint beta() { return 1; }\n}\n',
    # (declared in a.hpp, defined in a file of another name)
    'native/src/core/elsewhere.cpp': '#include "core/a.hpp"\nnamespace genko {\nint moved() { return 2; }\n}\n',
    'native/src/core/table.cpp': 'namespace genko {\nextern const int kTable[] = {1, 2};\n}\n',
    'native/src/core/pair.cpp': 'namespace genko {\nbool operator==(const Pair& a, const Pair& b) { return true; }\n}\n',
    # (a class in parts: window.hpp, window.cpp, window_part.cpp)
    'native/src/core/window.hpp': '#include <vector>\nnamespace genko {\nclass Window {\npublic:\n    Window();\n    void paint();\n    void part();\n};\n}\n',
    'native/src/core/window.cpp': '#include "core/window.hpp"\nnamespace genko {\nWindow::Window() {}\nvoid Window::paint() {}\n}\n',
    'native/src/core/window_part.cpp': '#include "core/window.hpp"\n#include "core/later.hpp"\nnamespace genko {\nvoid Window::part() {}\n}\n',
    'native/src/core/later.hpp': 'namespace genko {\nclass Later {\npublic:\n    void paint();\n};\n}\n',
    'native/src/core/later.cpp': '#include "core/later.hpp"\nnamespace genko {\nvoid Later::paint() {}\nstatic int alpha() { return 0; }\n}\n',
    # (no header says how it is reached)
    'native/src/core/lonely.cpp': 'namespace genko {\nint nobody_declares_this() { return 3; }\n}\n',
    # (the same name in another library)
    'native/src/render/CMakeLists.txt': 'genko_library(genko_render SOURCES draw.cpp)\n',
    'native/src/render/draw.hpp': 'namespace genko { int draw(); }\n',
    'native/src/render/draw.cpp': '#include "render/draw.hpp"\n#include "core/a.hpp"\nnamespace genko::render {\nint alpha() { return 9; }\n}\nint genko::draw() { return 0; }\n',
    'native/tests/support/testsupport.hpp': '#include "core/b.hpp"\nnamespace genko::test {\nQString genko_cli();\nRun run_genko(const QStringList& args);\nQString lock_helper();\nQString data(const QString& name);\nclass Holder {\npublic:\n    QString start();\n};\n}\n',
    'native/tests/support/testsupport.cpp': '#include "testsupport.hpp"\nnamespace genko::test {\nQString genko_cli() { return QString::fromUtf8(GENKO_CLI); }\nRun run_genko(const QStringList& args) {\n    return run(genko_cli(), args);\n}\nQString lock_helper() { return QString::fromUtf8(GENKO_LOCK_HELPER); }\nQString data(const QString& name) { return name; }\nQString Holder::start() { return lock_helper(); }\n}\n',
    'native/tests/support/lock_helper.cpp': '#include "core/window.hpp"\nint main() { genko::Window w; return 0; }\n',
    # (one of the always-run set, holding nothing of the product)
    'native/tests/unit/test_json.cpp': '#include <QtTest>\nvoid t() {}\n',
    'native/tests/unit/test_alpha.cpp': '#include <QtTest>\n#include "core/a.hpp"\nvoid t() { genko::alpha(); }\n',
    'native/tests/unit/test_beta.cpp': '#include "core/b.hpp"\n#include "../support/testsupport.hpp"\nvoid t() { genko::beta(); genko::test::data("x"); }\n',
    'native/tests/unit/beta_extra.c': 'int beta_extra(void) { return 0; }\n',
    'native/tests/e2e/test_cli.cpp': '#include "testsupport.hpp"\nvoid t() { genko::test::run_genko({"new"}); }\n',
    'native/tests/contract/test_contract_gamma.cpp': '#include "testsupport.hpp"\n#include "render/draw.hpp"\nvoid t() { genko::test::Holder h; h.start(); }\n',
    'native/tests/perf/test_perf_delta.cpp': '#include "core/b.hpp"\nvoid t() { genko::beta(); }\n',
}


def write_tree(folder, files, newline='\n'):
    for name, text in files.items():
        path = Path(folder) / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text.replace('\n', newline).encode('utf-8'))
    return Path(folder)


class MadeUpTree(unittest.TestCase):
    files = MADE_UP

    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.root = write_tree(self.folder.name, self.files)
        self.tree = policy.SourceTree(self.root)

    def closure(self, test):
        return self.tree.closure(self.tree.tests[test])

    def asan(self, paths, phase='integration', before=None):
        plan = policy.make_plan(paths, phase, self.root, before=before)
        rows = {r['preset']: r for r in plan['matrix']['include']}
        return plan, set(rows['linux-asan']['tests']) - policy.SAFETY if 'linux-asan' in rows else None


class ClosureRuleTests(MadeUpTree):
    def test_the_registration_is_read_from_cmake(self):
        self.assertEqual(self.tree.tests['test_beta'], ['native/tests/unit/beta_extra.c', 'native/tests/unit/test_beta.cpp'])
        self.assertEqual(sorted(self.tree.tests), ['test_alpha', 'test_beta', 'test_cli', 'test_contract_gamma', 'test_json', 'test_perf_delta'])
        self.assertEqual(self.tree.programs, {'genko': ['native/src/api/main.cpp'], 'genko_lock_helper': ['native/tests/support/lock_helper.cpp']})
        self.assertEqual(self.tree.macros, {'GENKO_CLI': 'genko', 'GENKO_LOCK_HELPER': 'genko_lock_helper'})

    def test_includes_are_followed_and_a_header_brings_what_defines_its_names(self):
        got = self.closure('test_alpha')
        # a.hpp, b.hpp through it, and the files that define what they declare — whatever their names
        for name in ('a.hpp', 'a.cpp', 'b.hpp', 'b.cpp', 'elsewhere.cpp', 'table.cpp', 'pair.cpp'):
            self.assertIn('native/src/core/' + name, got, name)
        self.assertIn('native/tests/unit/test_alpha.cpp', got)
        # not what it does not include, nor another library's file that defines a function of the same name
        for name in ('native/src/core/window.cpp', 'native/src/core/window_part.cpp', 'native/src/render/draw.cpp', 'native/src/api/cli.cpp',
                     'native/src/core/lonely.cpp', 'native/tests/support/testsupport.cpp'):
            self.assertNotIn(name, got, name)

    def test_a_class_declared_ahead_does_not_bring_its_files(self):
        # (a.hpp says `class Later;`: later.cpp defines Later::paint, which a.hpp does not declare — and a static alpha)
        self.assertNotIn('native/src/core/later.cpp', self.closure('test_alpha'))

    def test_a_class_in_parts_and_what_its_parts_include(self):
        got = self.tree.closure(['native/tests/support/lock_helper.cpp'])
        for name in ('window.hpp', 'window.cpp', 'window_part.cpp', 'later.hpp', 'later.cpp'):
            self.assertIn('native/src/core/' + name, got, name)
        self.assertNotIn('native/src/core/a.cpp', got)

    def test_includes_beside_the_file_and_from_the_two_roots(self):
        got = self.closure('test_beta')
        self.assertIn('native/tests/support/testsupport.hpp', got)   # ("../support/testsupport.hpp")
        self.assertIn('native/tests/support/testsupport.cpp', got)
        self.assertIn('native/src/core/b.cpp', got)                   # ("core/b.hpp" from native/src; "b.hpp" beside a.hpp)
        self.assertIn('native/tests/unit/beta_extra.c', got)
        self.assertIn('native/tests/support/testsupport.hpp', self.closure('test_cli'))  # ("testsupport.hpp" from tests/support)

    def test_a_programs_source_is_never_brought_by_a_header(self):
        # (main.cpp and the tests' own sources define names too: they belong to their program alone)
        for test in self.tree.tests:
            got = self.closure(test)
            self.assertNotIn('native/src/api/main.cpp', got)
            for other, sources in self.tree.tests.items():
                if other != test:
                    self.assertFalse(set(sources) & got, (test, other))

    def test_a_file_no_header_brings_is_not_explained(self):
        self.assertTrue(self.tree.unexplained('native/src/core/lonely.cpp'))
        self.assertFalse(self.tree.unexplained('native/src/core/elsewhere.cpp'))
        self.assertFalse(self.tree.unexplained('native/src/api/main.cpp'))
        plan, _ = self.asan(['native/src/core/lonely.cpp'])
        self.assertEqual(plan['tier'], 'full')
        self.assertIn('閉包で説明できないソース（どのヘッダーも連れて来ない）', plan['reasons'])

    def test_a_change_selects_the_tests_whose_closure_holds_it(self):
        self.assertEqual(self.asan(['native/src/core/window_part.cpp'])[1], {'test_contract_gamma'})  # (through genko_lock_helper, which it runs)
        # (test_cli: genko holds it; test_contract_gamma: draw.cpp includes a.hpp)
        self.assertEqual(self.asan(['native/src/core/elsewhere.cpp'])[1], {'test_alpha', 'test_cli', 'test_contract_gamma'})
        self.assertEqual(self.asan(['native/src/render/draw.cpp'])[1], {'test_contract_gamma'})
        self.assertEqual(self.asan(['native/tests/unit/beta_extra.c'])[1], {'test_beta'})
        self.assertEqual(self.asan(['native/tests/unit/test_alpha.cpp'])[1], {'test_alpha'})
        # every test's closure but test_json's: with the always-run set that is every test — the whole suite in every
        # configuration, for the reason shared code had
        plan, tests = self.asan(['native/src/core/b.hpp'])
        self.assertEqual(plan['tier'], 'full')
        self.assertIn('共通モデル・保存・権限・oracle境界', plan['reasons'])
        self.assertIn('関係する試験が全試験に及ぶ', plan['reasons'])
        self.assertTrue(all(r['suite'] == 'full' for r in plan['matrix']['include']))

    def test_a_changed_perf_test_asks_for_its_gate_and_is_not_a_normal_test(self):
        plan = policy.make_plan(['native/tests/perf/test_perf_delta.cpp'], 'development', self.root)
        self.assertIn('性能試験変更: 別の性能gateを必須', plan['reasons'])
        self.assertTrue(plan['performance_required'])
        self.assertFalse(any('test_perf_delta' in r['tests'] for r in plan['matrix']['include']))
        self.assertEqual(plan['tier'], 'related')

    def test_qt_meta_objects_of_one_library_come_together(self):
        files = dict(MADE_UP)
        files['native/src/app/one.hpp'] = 'class One : public QObject {\n    Q_OBJECT\npublic:\n    void first();\n};\n'
        files['native/src/app/one.cpp'] = '#include "app/one.hpp"\nvoid One::first() {}\n'
        files['native/src/app/two.hpp'] = 'class Two : public QObject {\n    Q_OBJECT\npublic slots:\n    void second();\n};\n'
        files['native/src/app/two.cpp'] = '#include "app/two.hpp"\nvoid Two::second() {}\n'
        files['native/src/app/plain.hpp'] = 'void plain();\n'
        files['native/src/app/plain.cpp'] = '#include "app/plain.hpp"\nvoid plain() {}\n'
        files['native/tests/unit/test_alpha.cpp'] += '#include "app/one.hpp"\n'
        with tempfile.TemporaryDirectory() as folder:
            tree = policy.SourceTree(write_tree(folder, files))
            got = tree.closure(tree.tests['test_alpha'])
            self.assertIn('native/src/app/two.cpp', got)   # (moc's one file refers to every Q_OBJECT class of the library)
            self.assertNotIn('native/src/app/plain.cpp', got)
            self.assertFalse(any(p.startswith('moc:') for p in got))


class ChildProcessTests(MadeUpTree):
    def test_programs_are_found_through_the_helpers_that_name_them(self):
        runs = self.tree.programs_of()
        self.assertEqual(runs['test_cli'], {'genko'})                       # (run_genko → genko_cli → GENKO_CLI)
        self.assertEqual(runs['test_contract_gamma'], {'genko_lock_helper'})  # (Holder::start → lock_helper → GENKO_LOCK_HELPER)
        # (test_beta includes the same support header and calls a helper that starts nothing)
        self.assertEqual(runs['test_beta'], set())
        self.assertEqual(runs['test_alpha'], set())
        self.assertEqual(self.tree.carriers['genko'][0], ['GENKO_CLI', 'genko_cli', 'run_genko'])
        self.assertEqual(self.tree.carriers['genko_lock_helper'][0], ['GENKO_LOCK_HELPER', 'Holder', 'lock_helper'])

    def test_a_test_that_runs_genko_is_related_to_all_genko_is_built_from(self):
        self.assertEqual(self.asan(['native/src/api/cli.cpp'])[1], {'test_cli'})
        self.assertEqual(self.asan(['native/src/api/main.cpp'])[1], {'test_cli'})
        self.assertIn('test_cli', self.asan(['native/src/core/a.cpp'])[1])
        self.assertEqual(self.asan(['native/tests/support/lock_helper.cpp'])[1], {'test_contract_gamma'})

    def test_the_macro_in_a_tests_own_source_and_add_dependencies(self):
        files = dict(MADE_UP)
        files['native/tests/unit/test_alpha.cpp'] += 'void s() { QProcess p; p.start(QStringLiteral(GENKO_CLI)); }\n'
        files['native/tests/CMakeLists.txt'] += 'target_compile_definitions(test_alpha PRIVATE GENKO_CLI="$<TARGET_FILE:genko>")\nadd_dependencies(test_beta genko)\n'
        with tempfile.TemporaryDirectory() as folder:
            runs = policy.SourceTree(write_tree(folder, files)).programs_of()
            self.assertEqual(runs['test_alpha'], {'genko'})
            self.assertEqual(runs['test_beta'], {'genko'})

    def test_a_helper_program_that_starts_genko(self):
        files = dict(MADE_UP)
        files['native/tests/support/lock_helper.cpp'] += 'void again() { QProcess::execute(QStringLiteral(GENKO_CLI)); }\n'
        with tempfile.TemporaryDirectory() as folder:
            tree = policy.SourceTree(write_tree(folder, files))
            self.assertEqual(tree.programs_of()['test_contract_gamma'], {'genko_lock_helper', 'genko'})
            self.assertEqual(tree.programs_of()['test_beta'], set())
            self.assertEqual(tree.carriers['genko'][0], ['GENKO_CLI', 'genko_cli', 'run_genko'])   # (the helper's own functions are not helpers of the tests)

    def test_a_use_whose_owner_cannot_be_read_counts_for_the_whole_file(self):
        files = dict(MADE_UP)
        # (the macro handed to another macro at file scope: no function to name)
        files['native/tests/support/odd.hpp'] = 'REGISTER_PROGRAM(GENKO_CLI);\ninline int odd() { return 1; }\n'
        files['native/tests/unit/test_alpha.cpp'] += '#include "odd.hpp"\n'
        with tempfile.TemporaryDirectory() as folder:
            tree = policy.SourceTree(write_tree(folder, files))
            self.assertEqual(tree.programs_of()['test_alpha'], {'genko'})
            self.assertEqual(tree.carriers['genko'][1], ['native/tests/support/odd.hpp'])

    def test_what_uses_a_name(self):
        text = (
            '#define RUN_IT run_cli(CLI)\n'
            'namespace a {\n'
            'QString cli();\n'                                    # a declaration: not a use
            'QString cli() { return CLI; }\n'
            'const QString kProgram = cli();\n'
            'class Server {\n    void go() { start(kProgram); }\n};\n'
            'void Server::stop() { cli(); }\n'
            'int wait(int ms = timeout(cli())) { return ms; }\n'
            '// CLI in a comment\n'
            'const char* words = "CLI in a string";\n'
            'int other() { return 0; }\n'
            '}\n')
        names, unknown = policy.enclosing_names(text, {'CLI', 'cli', 'kProgram'}, {'CLI'}, {'Server'})
        self.assertEqual(names, {'RUN_IT', 'cli', 'kProgram', 'Server', 'wait'})
        self.assertFalse(unknown)
        self.assertEqual(policy.enclosing_names('STATIC_CHECK(CLI);\n', {'CLI'}, {'CLI'}, set()), (set(), True))


DATA = dict(MADE_UP)
DATA.update({
    'native/src/core/CMakeLists.txt': (
        'set(GENKO_OPS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/data/ops.json")  # embedded\n'
        'file(READ "${GENKO_OPS_FILE}" _hex HEX)\n'
        'configure_file(ops_data.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/ops_data.cpp" @ONLY)\n'
        'foreach(_id IN LISTS GENKO_IDS)\n  file(READ "${CMAKE_CURRENT_SOURCE_DIR}/data/profiles/${_id}.icc" _hex HEX)\nendforeach()\n'
        'configure_file(profiles_data.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/profiles_data.cpp" @ONLY)\n'
        'set(GENKO_NOTES "${GENKO_SOURCE_ROOT}/resources/notes.txt")\n'
        'qt_add_resources(GENKO_PICTURES "${GENKO_SOURCE_ROOT}/resources/genko_pictures.qrc")\n'
        'genko_library(genko_core SOURCES a.cpp b.cpp "${CMAKE_CURRENT_BINARY_DIR}/ops_data.cpp")\n'),
    'native/src/core/ops_data.cpp.in': 'namespace genko::detail {\nextern const unsigned char kOpsJson[] = {\n@GENKO_OPS_BYTES@\n};\nextern const std::size_t kOpsJsonSize = sizeof(kOpsJson);\n}\n',
    'native/src/core/profiles_data.cpp.in': 'namespace genko::detail {\n@GENKO_PROFILES_DATA@\n}\n',
    'native/src/core/data/ops.json': '{}\n',
    'native/src/core/data/profiles/grey.icc': 'icc',
    'native/src/core/data/unused.bin': 'x',
    # (elsewhere.cpp reads the embedded table; test_beta does not hold elsewhere.cpp)
    'native/src/core/elsewhere.cpp': MADE_UP['native/src/core/elsewhere.cpp'] + 'namespace genko::detail { extern const unsigned char kOpsJson[]; }\n',
    'native/resources/genko_pictures.qrc': '<RCC>\n  <qresource prefix="/genko">\n    <file alias="one">pictures/one.png</file>\n  </qresource>\n</RCC>\n',
    'native/resources/genko_unused.qrc': '<RCC><qresource><file>pictures/two.png</file></qresource></RCC>\n',
    'native/resources/pictures/one.png': 'png',
    'native/resources/pictures/two.png': 'png',
    'native/resources/pictures/three.png': 'png',
    'native/resources/notes.txt': 'notes',
    'native/resources/pictures/README.md': 'about the pictures',
    'native/src/render/draw.cpp': MADE_UP['native/src/render/draw.cpp'] + 'static void pictures() { Q_INIT_RESOURCE(genko_pictures); }\n',
    'native/tests/data/tables/first.json': '{}',
    'native/tests/data/books/book-v3.genko/project.json': '{}',
    'native/tests/data/kinds/plain.json': '{}',
    'native/tests/data/kinds/other.json': '{}',
    'native/tests/data/nobody/reads.bin': 'x',
    'native/tests/data/NOTES.md': 'notes',
    'docs/ops.schema.json': '{}',
    'docs/GUIDE.md': 'guide',
    'integrations/agent/config.yaml': 'a: 1',
    'integrations/agent/first.json': '{}',
    'integrations/agent/client.py': 'pass',
    'native/resources/pictures/first.json': '{}',
    'native/tests/unit/test_alpha.cpp': MADE_UP['native/tests/unit/test_alpha.cpp'] + (
        'void d() {\n    read(test_data(QStringLiteral("tables/first.json")));\n    read(repo_root() + "/docs/ops.schema.json");\n    run(repo_root() + "/integrations/agent/client.py");\n'
        '    read(test_data("books/book-" + version + ".genko"));\n}\n'),
    'native/tests/unit/test_beta.cpp': MADE_UP['native/tests/unit/test_beta.cpp'] + (
        'void d() {\n    list(test_data(QStringLiteral("kinds/")));\n    read(fs::path(GENKO_TEST_DATA) / "tables" / "first.json");\n'
        '    read(test_data(QStringLiteral("kinds/%1").arg(name)));\n    Json j{{"kinds", 1}, {"tables", 2}};\n}\n'),
})


class DataRuleTests(MadeUpTree):
    files = DATA

    def test_data_embedded_by_cmake_goes_to_the_sources_that_read_its_names(self):
        self.assertEqual(self.tree.embedded('native/src/core/data/ops.json'), ({'native/src/core/ops_data.cpp.in'}, False))
        self.assertEqual(self.tree.consumers_of_template('native/src/core/ops_data.cpp.in'), {'native/src/core/elsewhere.cpp'})
        for path in ('native/src/core/data/ops.json', 'native/src/core/ops_data.cpp.in'):
            plan, tests = self.asan([path])
            self.assertEqual(tests, {'test_alpha', 'test_cli', 'test_contract_gamma'}, path)
            self.assertEqual(plan['tier'], 'integration')
        self.assertNotIn('native/src/core/elsewhere.cpp', self.closure('test_beta'))

    def test_embedded_data_the_rules_cannot_read_is_everything(self):
        cases = {
            'native/src/core/profiles_data.cpp.in': '埋め込みデータ（使うソースを読み取れない）',   # (its names are made by CMake)
            'native/src/core/data/profiles/grey.icc': '未分類native変更を安全側で全体へ拡大',        # (named in CMake by a pattern)
            'native/src/core/data/unused.bin': '未分類native変更を安全側で全体へ拡大',
            'native/resources/notes.txt': 'CMake が使うデータ（使い方を読み取れない）',               # (named in CMake, no template after it)
            'native/resources/pictures/three.png': '未分類native変更を安全側で全体へ拡大',           # (in no .qrc)
            'native/resources/pictures/two.png': 'Qt の resource（読み込むソースを読み取れない）',   # (its .qrc is initialised nowhere)
            'native/resources/gone.png': '消えた・名前の変わったデータ',
        }
        for path, reason in cases.items():
            plan, _ = self.asan([path])
            self.assertEqual(plan['tier'], 'full', path)
            self.assertIn(reason, plan['reasons'], path)

    def test_a_resource_goes_to_the_tests_that_hold_its_initialiser(self):
        self.assertEqual(sorted(self.tree.resources()), ['genko_pictures', 'genko_unused'])
        self.assertEqual(self.tree.resource_users('genko_pictures'), {'native/src/render/draw.cpp'})
        for path in ('native/resources/pictures/one.png', 'native/resources/genko_pictures.qrc'):
            self.assertEqual(self.asan([path])[1], {'test_contract_gamma'}, path)

    def test_test_data_goes_to_the_tests_that_name_it(self):
        self.assertEqual(self.asan(['native/tests/data/tables/first.json'])[1], {'test_alpha', 'test_beta'})  # (the path; the file's name)
        self.assertEqual(self.asan(['native/tests/data/books/book-v3.genko/project.json'])[1], {'test_alpha'})  # ("books/book-" + …)
        self.assertEqual(self.asan(['native/tests/data/kinds/plain.json'])[1], {'test_beta'})  # (the folder "kinds/", "kinds/%1")
        self.assertEqual(self.asan(['docs/ops.schema.json'])[1], {'test_alpha'})
        self.assertEqual(self.asan(['integrations/agent/client.py'])[1], {'test_alpha'})
        # (outside native/ and not a document: a file's name alone decides nothing — "first.json" is said by test_beta —,
        # and what the build takes in is never decided by a name)
        for path, reason in (('native/tests/data/nobody/reads.bin', '試験の木のデータ（読む試験を言い切れない）'), ('integrations/agent/config.yaml', '未分類変更'),
                             ('integrations/agent/first.json', '未分類変更'), ('native/resources/pictures/first.json', '未分類native変更を安全側で全体へ拡大')):
            plan, _ = self.asan([path])
            self.assertEqual(plan['tier'], 'full', path)
            self.assertIn(reason, plan['reasons'], path)

    def test_a_path_built_from_parts_names_the_folder_and_an_unsure_reader_is_everything(self):
        # (review P1-1: test_ledger_service_b reads root / "fixtures" / "studio" / "demo4" / ("p00" + n + ".json") — no
        # literal with a slash, none with the file's name — while other tests name the folder by its path)
        files = dict(DATA)
        files['native/tests/fixtures/studio/demo/p001.json'] = '{}'
        files['native/tests/fixtures/studio/demo/bible.json'] = '{}'
        files['native/tests/fixtures/elsewhere/p001.json'] = '{}'
        files['native/tests/unit/test_alpha.cpp'] += 'void f() { read(repo_root() + "/native/tests/fixtures/studio/demo/" + name); read(root() + "/native/tests/fixtures/studio/demo/bible.json"); }\n'
        files['native/tests/unit/test_beta.cpp'] += 'void f() { read(root / "native" / "tests" / "fixtures" / "studio" / "demo" / ("p00" + n + ".json")); }\n'
        with tempfile.TemporaryDirectory() as folder:
            root = write_tree(folder, files)
            tree = policy.SourceTree(root)
            self.assertEqual(tree.data_readers('native/tests/fixtures/studio/demo/p001.json'), ({'test_alpha', 'test_beta'}, True))
            plan = policy.make_plan(['native/tests/fixtures/studio/demo/p001.json'], 'development', root)
            for row in plan['matrix']['include']:
                if row['preset'].startswith('linux'):
                    self.assertIn('test_beta', row['tests'], row['preset'])
            # (a path that runs past the folder to another file reads that file, not this one)
            self.assertEqual(tree.data_readers('native/tests/fixtures/studio/demo/bible.json'), ({'test_alpha', 'test_beta'}, True))
            # a source that says a path ending at a folder of this name somewhere else: who reads the file cannot be told
            files['native/tests/e2e/test_cli.cpp'] += 'void g() { list(scratch + "/copies/demo"); }\n'
        with tempfile.TemporaryDirectory() as folder:
            root = write_tree(folder, files)
            tree = policy.SourceTree(root)
            tests, sure = tree.data_readers('native/tests/fixtures/studio/demo/p001.json')
            self.assertEqual((tests, sure), ({'test_alpha', 'test_beta'}, False))
            plan = policy.make_plan(['native/tests/fixtures/studio/demo/p001.json'], 'integration', root)
            self.assertEqual(plan['tier'], 'full')
            self.assertIn('試験の木のデータ（読む試験を言い切れない）', plan['reasons'])

    def test_a_folder_named_alone_reads_what_is_in_it(self):
        # ({"tables", 2} in test_beta: a word equal to the folder right above the file counts as naming the folder —
        # more tests than need be, never fewer; "/package" in one test no longer names every path with a part "package")
        self.assertEqual(self.tree.data_readers('native/tests/data/tables/second.json'), ({'test_beta'}, True))
        self.assertEqual(self.tree.files_naming('native/tests/data/tables/second.json', by_name=True), set())
        files = dict(DATA)
        files['native/tests/unit/test_alpha.cpp'] += 'void h() { open(dir + "/unit"); }\n'
        with tempfile.TemporaryDirectory() as folder:
            root = write_tree(folder, files)
            tree = policy.SourceTree(root)
            kind, tests, _ = policy.classify('native/tests/unit/beta_extra.c', lambda: tree, policy.inventory(root))
            self.assertEqual((kind, tests), ('related', {'test_beta'}))

    def test_documents_no_test_names_start_nothing(self):
        for path in ('docs/GUIDE.md', 'native/tests/data/NOTES.md', 'native/resources/pictures/README.md', 'README.md'):
            plan = policy.make_plan([path], 'integration', self.root)
            self.assertEqual((plan['tier'], plan['matrix']['include']), ('documents', []), path)


class BroadFallbackTests(MadeUpTree):
    def test_what_the_rules_do_not_understand_is_everything(self):
        cases = {
            'native/CMakeLists.txt': 'ビルド・依存・配布・登録の共通境界',
            'native/CMakePresets.json': 'ビルド・依存・配布・登録の共通境界',
            'native/vcpkg.json': 'ビルド・依存・配布・登録の共通境界',
            'native/cmake/GenkoFlags.cmake': 'ビルド・依存・配布・登録の共通境界',
            'native/third_party/pillow/libImaging/Geometry.c': 'ビルド・依存・配布・登録の共通境界',
            '.gitattributes': 'ビルド・依存・配布・登録の共通境界',
            'native/tests/CMakeLists.txt': 'ビルド・依存・配布・登録の共通境界',        # (no base to compare with)
            'native/src/core/CMakeLists.txt': 'ビルド・依存・配布・登録の共通境界',
            'tools/migration/pyref_tools.py': '共通モデル・保存・権限・oracle境界',
            'tools/acceptance/m3a_access_check.py': '共通モデル・保存・権限・oracle境界',
            'native/wasm-host/src/main.rs': '未分類native変更を安全側で全体へ拡大',
            'native/package/install.sh': '未分類native変更を安全側で全体へ拡大',
            'native/src/core/gone.cpp': '消えた・名前の変わったソース（今の木から閉包を作れない）',
            'native/tests/unit/test_removed.cpp': '消えた・名前の変わったソース（今の木から閉包を作れない）',
            'native/src/core/lonely.cpp': '閉包で説明できないソース（どのヘッダーも連れて来ない）',
            'tools/future-validation.py': '未分類変更',
            '.github/future-validation.json': '未分類変更',
            'src/genko/old.py': '未分類変更',
        }
        for path, reason in cases.items():
            for phase in ('development', 'integration', 'milestone'):
                plan = policy.make_plan([path], phase, self.root)
                self.assertEqual(plan['tier'], 'full', path)
                self.assertIn(reason, plan['reasons'], path)
                self.assertTrue(all(r['suite'] == 'full' for r in plan['matrix']['include']), path)
                self.assertTrue(plan['classification'][path].startswith('broad: ' + reason), path)

    def test_a_header_nothing_includes_is_everything(self):
        files = dict(MADE_UP)
        files['native/src/core/unused.hpp'] = 'int unused();\n'
        with tempfile.TemporaryDirectory() as folder:
            plan = policy.make_plan(['native/src/core/unused.hpp'], 'integration', write_tree(folder, files))
            self.assertEqual(plan['tier'], 'full')
            self.assertIn('どの試験の閉包にも入らないソース', plan['reasons'])

    def test_no_diff_is_everything_and_ci_changes_keep_their_tier(self):
        plan = policy.make_plan([], 'integration', self.root)
        self.assertEqual(plan['tier'], 'full')
        self.assertIn('差分なし・基点不明: 全体へ拡大', plan['reasons'])
        plan = policy.make_plan(['tools/ci/validation_run.py'], 'integration', self.root)
        self.assertEqual(plan['tier'], 'infrastructure')
        self.assertIn('CI自身の変更: 実ビルド・非契約全試験・両OSで確認', plan['reasons'])

    def test_one_unknown_path_widens_a_diff_of_known_ones(self):
        plan, tests = self.asan(['native/src/render/draw.cpp', 'native/package/install.sh'])
        self.assertEqual(plan['tier'], 'full')
        self.assertEqual(tests, {'test_alpha', 'test_beta', 'test_cli', 'test_contract_gamma'})


class CMakeChangeTests(MadeUpTree):
    def before(self, files):
        return lambda path: files.get(path)

    def test_a_test_added_or_changed_in_the_registration_is_that_test(self):
        old = {'native/tests/CMakeLists.txt': SUITE.replace('genko_test(test_beta SOURCES unit/test_beta.cpp unit/beta_extra.c DEPS genko_core LABELS unit)\n', '')
               .replace('TIMEOUT 900', 'TIMEOUT 600')}
        plan, tests = self.asan(['native/tests/CMakeLists.txt'], before=self.before(old))
        self.assertEqual(tests, {'test_beta', 'test_contract_gamma'})
        self.assertEqual(plan['tier'], 'integration')
        self.assertIn('related: 試験の登録: 文の変わった試験だけ（2 試験）', plan['classification']['native/tests/CMakeLists.txt'])
        # (a comment or blank lines alone: nothing; a statement about a test put right after that test's own: that test)
        same = {'native/tests/CMakeLists.txt': '# a note\n\n' + SUITE.replace('# runs genko (a helper)', '')}
        self.assertEqual(self.asan(['native/tests/CMakeLists.txt'], before=self.before(same))[1], set())
        alpha = 'genko_test(test_alpha SOURCES unit/test_alpha.cpp DEPS genko_core LABELS unit)\n'
        self.assertEqual(policy.changed_tests_of_suite(SUITE, SUITE.replace(alpha, alpha + 'set_tests_properties(test_alpha PROPERTIES RUN_SERIAL TRUE)\n')), {'test_alpha'})
        self.assertEqual(policy.changed_tests_of_suite(SUITE.replace('\n', '\r\n'), SUITE), set())
        # (a new test wherever it is put among the statements that are there, with its own statements)
        base = SUITE + 'if(GENKO_BUILD_APP)\n  set(X 1)\nendif()\n'
        added = base.replace('  set(X 1)\n', '  set(X 1)\n  genko_test(test_new SOURCES unit/test_new.cpp LABELS unit)\n  add_dependencies(test_new genko)\n')
        self.assertEqual(policy.changed_tests_of_suite(base, added), {'test_new'})
        self.assertEqual(policy.changed_tests_of_suite(SUITE, SUITE + 'genko_test(test_new SOURCES unit/test_new.cpp LABELS unit)\n'), {'test_new'})
        # (a new block around it is a change of the shared part)
        self.assertIsNone(policy.changed_tests_of_suite(SUITE, SUITE + 'if(UNIX)\n  genko_test(test_new SOURCES unit/test_new.cpp LABELS unit)\nendif()\n'))

    def test_a_statement_about_several_tests_belongs_to_each(self):
        # (review P3: set_tests_properties(a b PROPERTIES …) and set_property(TEST a b …) were given to the first name only)
        alpha = 'genko_test(test_alpha SOURCES unit/test_alpha.cpp DEPS genko_core LABELS unit)\n'
        both = SUITE.replace(alpha, alpha + 'set_tests_properties(test_alpha test_beta PROPERTIES TIMEOUT 100)\nset_property(TEST test_alpha test_cli APPEND PROPERTY ENVIRONMENT "A=1")\n')
        changed = both.replace('TIMEOUT 100', 'TIMEOUT 200').replace('A=1', 'A=2')
        self.assertEqual(policy.changed_tests_of_suite(both, changed), {'test_alpha', 'test_beta', 'test_cli'})
        self.assertEqual(policy.changed_tests_of_suite(both, both), set())

    def test_a_moved_or_removed_registration_is_everything(self):
        # (review P1-3a: the statements of the tests were cut out and the rest compared, so where a statement stood was
        # not seen — a test moved into if(MSVC) is no longer registered on Linux, and read as "nothing changed")
        alpha = 'genko_test(test_alpha SOURCES unit/test_alpha.cpp DEPS genko_core LABELS unit)\n'
        blocked = SUITE.replace(alpha, '') + 'if(MSVC)\n  set(X 1)\n  ' + alpha + 'endif()\n'
        before_block = SUITE + 'if(MSVC)\n  set(X 1)\nendif()\n'
        for old, new in ((before_block, blocked), (blocked, before_block),
                         (SUITE, SUITE.replace(alpha, '')),                                    # (the registration gone)
                         (SUITE, SUITE.replace(alpha, '') + alpha),                             # (moved to the end)
                         (SUITE, SUITE + 'set_tests_properties(test_alpha PROPERTIES RUN_SERIAL TRUE)\n'),   # (a statement far from its test)
                         (SUITE + 'add_dependencies(test_cli genko)\n', SUITE)):
            self.assertIsNone(policy.changed_tests_of_suite(old, new), new[-120:])
        files = dict(MADE_UP)
        files['native/tests/CMakeLists.txt'] = blocked
        with tempfile.TemporaryDirectory() as folder:
            plan = policy.make_plan(['native/tests/CMakeLists.txt'], 'milestone', write_tree(folder, files), before=lambda path: before_block)
            self.assertEqual(plan['tier'], 'full')
            self.assertTrue(all(r['suite'] == 'full' for r in plan['matrix']['include']))

    def test_target_sources_of_a_test_are_its_sources(self):
        # (review P3: a test given more sources by target_sources had them outside its closure)
        files = dict(MADE_UP)
        files['native/tests/CMakeLists.txt'] += 'target_sources(test_alpha PRIVATE unit/alpha_more.cpp)\n'
        files['native/tests/unit/alpha_more.cpp'] = '#include "core/window.hpp"\nvoid more() {}\n'
        with tempfile.TemporaryDirectory() as folder:
            root = write_tree(folder, files)
            tree = policy.SourceTree(root)
            self.assertEqual(tree.tests['test_alpha'], ['native/tests/unit/alpha_more.cpp', 'native/tests/unit/test_alpha.cpp'])
            self.assertIn('native/src/core/window_part.cpp', tree.closure(tree.tests['test_alpha']))
            self.assertIn('test_alpha', tree.tests_reaching('native/src/core/window.cpp'))
            self.assertIn('native/tests/unit/alpha_more.cpp', tree.executable_sources)

    def test_a_change_of_the_shared_part_of_the_registration_is_everything(self):
        for old in (SUITE.replace('support/lock_helper.cpp', 'support/other_helper.cpp'),
                    SUITE.replace('GENKO_CLI="$<TARGET_FILE:genko>"\n', ''),
                    SUITE + 'if(MSVC)\n  set_source_files_properties(unit/beta_extra.c PROPERTIES COMPILE_OPTIONS "/W0")\nendif()\n',
                    SUITE + 'foreach(slow test_alpha)\nendforeach()\n'):
            self.assertIsNone(policy.changed_tests_of_suite(old, SUITE))
            plan, _ = self.asan(['native/tests/CMakeLists.txt'], before=self.before({'native/tests/CMakeLists.txt': old}))
            self.assertEqual(plan['tier'], 'full')
        # (the file was not there at the base)
        self.assertEqual(self.asan(['native/tests/CMakeLists.txt'], before=self.before({}))[0]['tier'], 'full')

    def test_a_librarys_source_list_alone_is_the_sources_added(self):
        now = MADE_UP['native/src/core/CMakeLists.txt']
        old = {'native/src/core/CMakeLists.txt': now.replace('    elsewhere.cpp\n', '').replace('    window.cpp\n', '    window.cpp  # the window\n    removed_file.cpp\n')}
        self.assertEqual(policy.changed_sources_of_library(old['native/src/core/CMakeLists.txt'], now), {'elsewhere.cpp', 'removed_file.cpp'})
        plan, tests = self.asan(['native/src/core/CMakeLists.txt'], before=self.before(old))
        self.assertEqual(tests, {'test_alpha', 'test_cli', 'test_contract_gamma'})   # (the closure of elsewhere.cpp; removed_file.cpp is gone)
        # anything else in the file: everything
        for other in (now.replace('genko_library(genko_core', 'genko_library(genko_core2'), now + 'target_compile_definitions(genko_core PUBLIC X=1)\n',
                      now.replace('a.cpp', '"${CMAKE_CURRENT_BINARY_DIR}/a.cpp"')):
            self.assertIsNone(policy.changed_sources_of_library(other, now))
            self.assertEqual(self.asan(['native/src/core/CMakeLists.txt'], before=self.before({'native/src/core/CMakeLists.txt': other}))[0]['tier'], 'full')
        # a source added that no header brings: not explained, everything
        lonely = {'native/src/core/CMakeLists.txt': now.replace('    lonely.cpp\n', '')}
        plan, _ = self.asan(['native/src/core/CMakeLists.txt'], before=self.before(lonely))
        self.assertEqual(plan['tier'], 'full')

    def test_a_source_named_outside_the_list_or_moved_between_lists_is_everything(self):
        # (review P1-3b: every *.cpp word of the file was taken as "the list" — a source added to
        # set_source_files_properties(… COMPILE_OPTIONS "-fno-builtin"), the floating-point code's options, read as
        # "0 tests"; and a source moved between two libraries of one folder)
        base = ('genko_library(genko_render\n  SOURCES\n    a.cpp\n    b.cpp\n    npcompat.cpp\n  PUBLIC_DEPS genko_core)\n'
                'if(NOT MSVC)\n  set_source_files_properties(npcompat.cpp PROPERTIES COMPILE_OPTIONS "-fno-builtin")\nendif()\n'
                'set(GENKO_RENDER_OPS_SOURCES ops_registry.cpp ops_tones.cpp)\n'
                'genko_library(genko_render_ops SOURCES ${GENKO_RENDER_OPS_SOURCES} PUBLIC_DEPS genko_render)\n'
                'add_executable(tool tool_main.cpp)\n')
        self.assertEqual(policy.changed_sources_of_library(base, base), set())
        self.assertEqual(policy.changed_sources_of_library(base, base.replace('    b.cpp\n', '    b.cpp\n    c.cpp\n')), {'c.cpp'})
        self.assertEqual(policy.changed_sources_of_library(base, base.replace('ops_tones.cpp)', 'ops_tones.cpp ops_3d.cpp)')), {'ops_3d.cpp'})
        for other in (base.replace('set_source_files_properties(npcompat.cpp', 'set_source_files_properties(npcompat.cpp b.cpp'),   # (compile options)
                      base.replace('    b.cpp\n', '').replace('ops_tones.cpp)', 'ops_tones.cpp b.cpp)'),                          # (moved to the other library)
                      base.replace('tool_main.cpp', 'tool_main.cpp extra.cpp'),                                                     # (a program's sources)
                      base.replace('PUBLIC_DEPS genko_core', 'PUBLIC_DEPS genko_core extra.cpp'),                                   # (after the list)
                      base + 'set(GENKO_MORE_SOURCES d.cpp)\n',                                                                    # (another list)
                      base + 'genko_library(genko_render SOURCES z.cpp)\n'):                                                       # (two lists of one name)
            self.assertIsNone(policy.changed_sources_of_library(base, other), other[-100:])
            self.assertIsNone(policy.changed_sources_of_library(other, base), other[-100:])
        # the real files read as lists, with their compile-option statements outside them
        for library, inside, outside in (('render', 'npcompat.cpp', 'set_source_files_properties(npcompat.cpp PROPERTIES'),
                                         ('core', 'pymath.cpp', 'set_source_files_properties(pymath.cpp linalg3.cpp PROPERTIES')):
            text = (ROOT / 'native/src' / library / 'CMakeLists.txt').read_text(encoding='utf-8')
            lists, rest = policy.library_sources(text)
            self.assertIn(inside, lists['genko_library genko_' + library])
            self.assertIn(outside, rest)
            self.assertIsNone(policy.changed_sources_of_library(text, text.replace(outside, outside.replace(' PROPERTIES', ' resample.cpp PROPERTIES'))))
            self.assertEqual(policy.changed_sources_of_library(text, text.replace('    ' + inside + '\n', '    ' + inside + '\n    brand_new.cpp\n')), {'brand_new.cpp'})
        text = (ROOT / 'native/src/render/CMakeLists.txt').read_text(encoding='utf-8')
        moved = text.replace('set(GENKO_RENDER_OPS_SOURCES ops_registry.cpp', 'set(GENKO_RENDER_OPS_SOURCES npcompat.cpp ops_registry.cpp').replace('    npcompat.cpp\n', '', 1)
        self.assertNotEqual(moved, text)
        self.assertIsNone(policy.changed_sources_of_library(text, moved))


class RenameTests(unittest.TestCase):
    def test_a_renamed_file_is_its_old_path_gone(self):
        # (review P1-2: git's rename detection listed only the new name, so "a path that is gone → everything" never fired:
        # a picture renamed for one test, still read under its old name by another, went to the tests naming the folder)
        files = dict(DATA)
        files['native/tests/data/tables/first.json'] = '{"rows": [' + ', '.join(str(i) for i in range(400)) + ']}\n'
        files['native/src/core/later.hpp'] = MADE_UP['native/src/core/later.hpp']
        with tempfile.TemporaryDirectory() as folder:
            root = write_tree(folder, files)

            def git(*args):
                return subprocess.run(['git', '-c', 'user.name=t', '-c', 'user.email=t@example.invalid', '-c', 'commit.gpgsign=false', *args],
                                      cwd=root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.decode()
            git('init', '-q')
            git('add', '-A')
            git('commit', '-q', '-m', 'base')
            base = git('rev-parse', 'HEAD').strip()
            git('mv', 'native/tests/data/tables/first.json', 'native/tests/data/tables/first-v2.json')
            git('mv', 'native/src/core/later.hpp', 'native/src/core/afterwards.hpp')
            git('commit', '-q', '-m', 'renamed')
            # (git itself calls both renames)
            self.assertEqual(git('diff', '--name-status', '-M', base, 'HEAD').count('R100'), 2)
            paths = policy.changed_paths(root, base, 'HEAD')
            self.assertEqual(sorted(paths), ['native/src/core/afterwards.hpp', 'native/src/core/later.hpp',
                                             'native/tests/data/tables/first-v2.json', 'native/tests/data/tables/first.json'])
            for phase in ('development', 'integration', 'milestone'):
                for changed in (paths, [p for p in paths if 'tables' in p], [p for p in paths if 'core' in p]):
                    plan = policy.make_plan(changed, phase, root)
                    self.assertEqual(plan['tier'], 'full', (phase, changed))
                    self.assertTrue(all(r['suite'] == 'full' for r in plan['matrix']['include']))
            plan = policy.make_plan(paths, 'integration', root)
            self.assertEqual(plan['classification']['native/tests/data/tables/first.json'], 'broad: 試験の木のデータ（読む試験を言い切れない）')
            self.assertEqual(plan['classification']['native/src/core/later.hpp'], 'broad: 消えた・名前の変わったソース（今の木から閉包を作れない）')


class StageExitTests(MadeUpTree):
    def rows(self, paths, phase):
        plan = policy.make_plan(paths, phase, self.root)
        return plan, {r['preset']: r for r in plan['matrix']['include']}

    def test_milestone_and_final(self):
        everything = {'test_alpha', 'test_beta', 'test_cli', 'test_contract_gamma', 'test_json'}
        plan, rows = self.rows(['native/src/render/draw.cpp'], 'milestone')
        self.assertEqual(sorted(rows), ['linux-asan', 'linux-release', 'windows-debug', 'windows-release'])
        self.assertEqual((rows['linux-release']['suite'], set(rows['linux-release']['tests'])), ('full', everything))
        self.assertEqual((rows['linux-asan']['suite'], rows['linux-asan']['tests']), ('related', ['test_contract_gamma', 'test_json']))
        self.assertEqual((rows['windows-debug']['tests'], rows['windows-debug']['excluded_contracts']), (['test_json'], ['test_contract_gamma']))
        self.assertEqual(plan['tier'], 'milestone')
        plan, rows = self.rows(['native/src/render/draw.cpp'], 'final')
        self.assertEqual(sorted(rows), ['linux-asan', 'linux-debug', 'linux-release', 'windows-debug', 'windows-release'])
        for name, row in rows.items():
            self.assertEqual(row['suite'], 'full')
            self.assertEqual(set(row['tests']), everything - ({'test_contract_gamma'} if name.startswith('windows') else set()))
        self.assertEqual(plan['tier'], 'full')
        # (the same diff as an integration candidate: as before)
        plan, rows = self.rows(['native/src/render/draw.cpp'], 'integration')
        self.assertEqual(sorted(rows), ['linux-asan', 'linux-release', 'windows-debug', 'windows-release'])
        self.assertEqual(plan['tier'], 'integration')
        self.assertFalse(plan['performance_required'])

    def test_everything_can_be_asked_for_without_adding_a_configuration(self):
        # (while validation_closure_check.py fails, the classification is not relied on: the same configurations, every test)
        plan = policy.make_plan(['native/src/render/draw.cpp'], 'milestone', self.root, all_tests=True)
        rows = {r['preset']: r for r in plan['matrix']['include']}
        self.assertEqual(sorted(rows), ['linux-asan', 'linux-release', 'windows-debug', 'windows-release'])
        self.assertTrue(all(r['suite'] == 'full' for r in rows.values()))
        self.assertEqual(set(rows['linux-asan']['tests']), {'test_alpha', 'test_beta', 'test_cli', 'test_contract_gamma', 'test_json'})
        self.assertIn('指定（--all-tests）: 分類によらず全体へ拡大', plan['reasons'])
        self.assertEqual(plan['tier'], 'full')
        self.assertEqual(policy.make_plan(['docs/x.md'], 'development', self.root, all_tests=True)['tier'], 'full')

    def test_a_ci_change_at_a_stage_exit(self):
        plan, rows = self.rows(['tools/ci/validation_run.py', 'native/src/render/draw.cpp'], 'milestone')
        self.assertNotIn('linux-debug', rows)
        self.assertEqual(set(rows['linux-asan']['tests']), {'test_alpha', 'test_beta', 'test_cli', 'test_contract_gamma', 'test_json'})  # (the normal tests, and the contract the diff reaches)
        other, rows = self.rows(['tools/ci/validation_run.py', 'native/src/api/cli.cpp'], 'milestone')
        self.assertNotIn('test_contract_gamma', rows['linux-asan']['tests'])
        self.assertIn('test_contract_gamma', rows['linux-release']['tests'])
        self.assertIn('CI自身の変更: 実ビルド・非契約全試験・両OSで確認', plan['reasons'])

    def test_the_plan_is_the_same_whatever_the_checkout(self):
        paths = ['native/src/core/elsewhere.cpp', 'native/tests/unit/beta_extra.c', 'docs/x.md']
        first = policy.make_plan(paths, 'milestone', self.root)
        self.assertEqual(json.dumps(first, sort_keys=True), json.dumps(policy.make_plan(list(reversed(paths)) + paths, 'milestone', self.root), sort_keys=True))
        # (a Windows checkout: CRLF in every file, the paths with backslashes)
        with tempfile.TemporaryDirectory() as folder:
            windows = write_tree(folder, MADE_UP, newline='\r\n')
            again = policy.make_plan([p.replace('/', '\\') for p in paths], 'milestone', windows)
            self.assertEqual(json.dumps(first, sort_keys=True), json.dumps(again, sort_keys=True))
            tree = policy.SourceTree(windows)
            self.assertEqual({t: sorted(self.tree.closure(s)) for t, s in self.tree.tests.items()}, {t: sorted(tree.closure(s)) for t, s in tree.tests.items()})
            self.assertEqual(self.tree.programs_of(), tree.programs_of())

    def test_an_include_spelt_in_another_case_is_not_followed_anywhere(self):
        # (a Windows disk would find "Core/A.hpp"; the tree's own spelling decides, the same on every system)
        files = dict(MADE_UP)
        files['native/tests/unit/test_alpha.cpp'] = '#include "Core/A.hpp"\nvoid t() {}\n'
        with tempfile.TemporaryDirectory() as folder:
            tree = policy.SourceTree(write_tree(folder, files))
            self.assertEqual(tree.closure(tree.tests['test_alpha']), {'native/tests/unit/test_alpha.cpp'})
            self.assertIn('native/src/core/a.cpp', self.closure('test_alpha'))

    def test_the_tree_is_read_again_when_it_changes(self):
        first = policy.source_tree(self.root)
        self.assertIs(policy.source_tree(self.root), first)
        (self.root / 'native/src/core/new.hpp').write_text('int more();\n', encoding='utf-8')
        second = policy.source_tree(self.root)
        self.assertIsNot(second, first)
        self.assertIn('native/src/core/new.hpp', second.text)


def selection_of_2026_11_13(paths, phase, inv):
    """What the policy of da6acebd (2026-11-13) selected: configuration → tests. Its rules written out here, by folder and by
    name, so the comparison does not depend on the code it is compared with."""
    everything = inv['normal'] | inv['contract']
    related = set(policy.SAFETY)
    broad = not paths
    infrastructure = native_change = False
    for path in sorted(set(paths)):
        if path.startswith('docs/') or path.endswith('.md'):
            continue
        if path == '.github/workflows/native.yml' or path.startswith('tools/ci/'):
            infrastructure = True
        elif path.startswith(('native/cmake/', 'native/vendor/', 'native/third_party/', 'native/packaging/')) or path in (
                'native/CMakeLists.txt', 'native/CMakePresets.json', 'native/vcpkg.json', 'native/tests/CMakeLists.txt', '.gitattributes'):
            broad = native_change = True
        elif path.startswith(('native/src/core/', 'native/src/storage/', 'native/tests/support/', 'tools/migration/', 'tools/acceptance/')):
            broad = native_change = True
        elif path.startswith('native/src/render/'):
            native_change = True
            name = path.rsplit('/', 1)[-1].rsplit('.', 1)[0]
            related |= {n for n in inv['normal'] if any(k in n for k in ('image', 'png', 'render', 'stroke', 'raster', 'ops', 'pynum', 'pyrandom'))}
            if name in ('raster_ops', 'selection'):
                related |= {n for n in everything if any(k in n for k in ('raster', 'select', 'mask'))}
            elif any(k in name for k in ('tone', 'effect', 'ruler')):
                related |= {n for n in everything if any(k in n for k in ('tone', 'ruler'))}
            elif any(k in name for k in ('3d', 'mannequin')):
                related |= {n for n in everything if '3d' in n}
            else:
                broad = True
        elif path.startswith('native/src/app/'):
            native_change = True
            related |= {n for n in everything if n.startswith('test_gui_') or n in ('test_session', 'test_ops_e2e')}
        elif path.startswith('native/tests/'):
            native_change = True
            name = path.rsplit('/', 1)[-1].rsplit('.', 1)[0]
            if name in everything:
                related.add(name)
            elif name not in inv['perf']:
                broad = True
        else:
            broad = native_change = True
    if phase == 'milestone':
        broad = native_change = True
    if not (broad or native_change or infrastructure):
        return {}
    if broad:
        presets = ['linux-release', 'linux-asan', 'windows-debug', 'windows-release'] + (['linux-debug'] if phase == 'milestone' else [])
    elif infrastructure:
        presets = ['linux-release', 'linux-asan', 'windows-debug', 'windows-release'] + (['linux-debug'] if native_change and phase == 'development' else [])
    else:
        presets = ['linux-debug', 'linux-asan', 'windows-debug'] if phase == 'development' else ['linux-release', 'linux-asan', 'windows-debug', 'windows-release']
    rows = {}
    for preset in presets:
        full = broad or (phase == 'integration' and preset == 'linux-release' and native_change)
        chosen = set(everything if full else (inv['normal'] | (related if native_change else set())) if infrastructure else related)
        if preset.startswith('windows'):
            chosen -= inv['contract']
        rows[preset] = chosen & everything
    return rows


class NeverMoreThanBeforeTests(unittest.TestCase):
    # (the owner's aim is fewer tests in a run: for the same diff and phase, no configuration is added and no
    # configuration runs a test the policy of 2026-11-13 did not run — but for the three cases below, where that policy
    # left out a test the changed file is built into or read by)
    AREAS = [
        'native/src/core/pynum.cpp', 'native/src/core/model.hpp', 'native/src/core/data/ops_schema.json', 'native/src/core/CMakeLists.txt',
        'native/src/storage/lock.cpp', 'native/src/storage/reader.cpp',
        'native/src/render/raster_ops.cpp', 'native/src/render/selection.cpp', 'native/src/render/tones.cpp', 'native/src/render/ops_effects.cpp',
        'native/src/render/ops_3d.cpp', 'native/src/render/page.cpp', 'native/src/render/text/balloons.cpp', 'native/src/render/draw.hpp',
        'native/src/render/CMakeLists.txt',
        'native/src/app/canvas.cpp', 'native/src/app/main_window.hpp', 'native/src/app/CMakeLists.txt',
        'native/src/service/letter.cpp', 'native/src/service/wording.cpp', 'native/src/api/cli.cpp', 'native/src/api/mcp_tools.cpp',
        'native/src/formats/pdf.cpp', 'native/src/studio/files.cpp',
        'native/resources/studio/layouts.json', 'native/resources/genko_materials.qrc', 'native/resources/fonts/Yomogi-Regular.ttf',
        'native/tests/support/testsupport.cpp', 'native/tests/support/testsupport.hpp', 'native/tests/gui/gui_support.hpp',
        'native/tests/unit/test_strokes.cpp', 'native/tests/unit/k1_probe.inc', 'native/tests/perf/test_perf_render.cpp',
        'native/tests/data/type/plain_lines.json', 'native/tests/e2e/refusal_cases.json', 'native/tests/CMakeLists.txt',
        'native/CMakeLists.txt', 'native/cmake/GenkoFlags.cmake', 'native/wasm-host/src/main.rs', 'native/package/install.sh',
        'tools/ci/validation_run.py', 'tools/migration/pyref_tools.py', '.github/workflows/native.yml',
        'docs/cpp-migration/PLAN.md', 'integrations/hermes/config.example.yaml', 'README.md',
    ]
    DIFFS = [
        ['native/src/app/canvas.cpp', 'native/src/render/raster_ops.cpp'],
        ['native/src/app/canvas.cpp', 'native/src/service/letter.cpp'],
        ['native/src/render/tones.cpp', 'native/src/render/ops_3d.cpp', 'native/tests/unit/test_strokes.cpp'],
        ['native/src/api/cli.cpp', 'tools/ci/validation_run.py'],
        ['native/src/core/model.hpp', 'native/src/storage/reader.cpp', 'native/src/app/canvas.cpp', 'native/tests/CMakeLists.txt'],
        ['docs/cpp-migration/PLAN.md', 'native/src/app/canvas.cpp'],
        [],
    ]

    @classmethod
    def setUpClass(cls):
        cls.inv = policy.inventory(ROOT)

    def compare(self, paths, phase):
        before = selection_of_2026_11_13(paths, 'milestone' if phase == 'final' else phase, self.inv)
        now = {r['preset']: set(r['tests']) for r in policy.make_plan(paths, phase, ROOT)['matrix']['include']}
        # every configuration both plans have: no test the old one did not run there (ASan is in every plan of both)
        self.assertEqual('linux-asan' in now, 'linux-asan' in before, (paths, phase))
        for preset, tests in now.items():
            if preset in before:
                self.assertLessEqual(tests, before[preset], (paths, phase, preset, sorted(tests - before[preset])[:8]))
        # and over all configurations, not more runs of tests. (A development diff the old policy sent to every test ran
        # Release, ASan and the two Windows builds in full; decided by the closure it is V1's Debug, ASan and Windows
        # Debug on the related tests — a configuration the old plan did not have, fewer runs in all.)
        self.assertLessEqual(sum(map(len, now.values())), sum(map(len, before.values())), (paths, phase))
        if phase != 'development':
            self.assertLessEqual(set(now), set(before), (paths, phase, 'a configuration the old policy did not run'))
        return before, now

    def test_one_changed_file_of_each_area(self):
        for path in self.AREAS:
            for phase in ('development', 'integration'):
                with self.subTest(path=path, phase=phase):
                    self.compare([path], phase)

    def test_diffs_of_several_areas(self):
        for paths in self.DIFFS:
            for phase in ('development', 'integration'):
                with self.subTest(paths=paths, phase=phase):
                    self.compare(paths, phase)

    def test_a_stage_exit_is_never_more_than_the_old_stage_exit_and_the_final_candidate_is_the_same(self):
        everything = self.inv['normal'] | self.inv['contract']
        for paths in [[p] for p in self.AREAS] + self.DIFFS:
            with self.subTest(paths=paths):
                before, now = self.compare(paths, 'milestone')
                self.assertNotIn('linux-debug', now)
                self.assertEqual(now['linux-release'], everything)
                final_before, final_now = self.compare(paths, 'final')
                self.assertEqual(final_now, final_before)

    def test_the_rules_by_names_are_exactly_the_old_ones(self):
        for path in ('native/src/app/canvas.cpp', 'native/src/app/CMakeLists.txt', 'native/src/render/raster_ops.cpp', 'native/src/render/selection.cpp',
                     'native/src/render/tones.cpp', 'native/src/render/ops_effects.cpp', 'native/src/render/ops_rulers.cpp', 'native/src/render/ops_3d.cpp',
                     'native/src/render/mannequin_draw.cpp'):
            for phase in ('development', 'integration'):
                before, now = self.compare([path], phase)
                self.assertEqual(now, before, (path, phase))
            plan = policy.make_plan([path], 'integration', ROOT)
            self.assertTrue(plan['classification'][path].startswith('related: 名前の規則（2026-11-13 までのまま）: '), plan['classification'][path])
        # (and where the closure decides, the plan says so)
        plan = policy.make_plan(['native/src/service/letter.cpp', 'native/src/render/page.cpp'], 'integration', ROOT)
        self.assertTrue(all(v.startswith('related: ソースの閉包（') for v in plan['classification'].values()), plan['classification'])

    def test_where_the_old_policy_left_out_a_test_the_file_reaches(self):
        # The three cases where more is selected than on 2026-11-13, each a test the old rule missed:
        def asan(path):
            before = selection_of_2026_11_13([path], 'integration', self.inv).get('linux-asan', set())
            rows = {r['preset']: set(r['tests']) for r in policy.make_plan([path], 'integration', ROOT)['matrix']['include']}
            return before, rows.get('linux-asan', set())
        # 1. the guides and tables of resources/studio are built into the service (studio_data.cpp); by their .md the
        #    old policy took a change of the guide for a document and built nothing
        before, now = asan('native/resources/studio/guide/SKILL.md')
        self.assertEqual(before, set())
        self.assertTrue({'test_service', 'test_ledger_service_b', 'test_mcp_api_e2e'} <= now)
        # 2. a document a test reads and compares: that test (the old policy: nothing)
        for path, test in (('docs/ops.schema.json', 'test_ledger_service_a'), ('docs/PSD_TEXT_SUPPORT.md', 'test_psd_text'),
                           ('docs/cpp-migration/THIRD_PARTY.md', 'test_package'), ('integrations/hermes/genko-manga/SKILL.md', 'test_ledger_service_b')):
            before, now = asan(path)
            self.assertEqual(before, set(), path)
            self.assertEqual(now - policy.SAFETY, {test}, path)
        # 3. one source that two tests are built from: both (the old policy took the one of the file's name)
        before, now = asan('native/tests/contract/test_contract_plugins.cpp')
        self.assertEqual(now - before, {'test_contract_wasm_plugins'})


class RealTreeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tree = policy.source_tree(ROOT)
        cls.inv = policy.inventory(ROOT)
        cls.everything = cls.inv['normal'] | cls.inv['contract']

    def reached(self, path):
        kind, tests, reason = policy.classify(path, lambda: self.tree, self.inv)
        return kind, tests & self.everything, reason

    def test_the_registration_and_the_inventory_agree(self):
        self.assertEqual(set(self.tree.tests), self.everything | self.inv['perf'])
        for name, sources in self.tree.tests.items():
            self.assertTrue(sources, name)
            for source in sources:
                self.assertIn(source, self.tree.text, name)
        self.assertEqual(self.tree.programs['genko'], ['native/src/api/main.cpp'])
        for program in ('genko_lock_helper', 'genko_argv_helper', 'genko_test_native_plugin', 'genko_fake_mcp'):
            self.assertIn(program, self.tree.programs)
        self.assertEqual({m: t for m, t in self.tree.macros.items() if t}, {
            'GENKO_CLI': 'genko', 'GENKO_LOCK_HELPER': 'genko_lock_helper', 'GENKO_ARGV_HELPER': 'genko_argv_helper',
            'GENKO_TEST_NATIVE_PLUGIN': 'genko_test_native_plugin', 'GENKO_FAKE_MCP': 'genko_fake_mcp'})

    def test_every_include_of_the_tree_is_found_or_is_not_the_trees(self):
        # (an include root added in CMake and not in INCLUDE_ROOTS would cut the closures short: every quoted include is
        # found, or is a file moc writes, or a header of the vendored libraries)
        vendored = {p.name for p in (ROOT / 'native/third_party').rglob('*.h')}
        made = ' '.join(p.read_text(encoding='utf-8') for p in (ROOT / 'native/third_party').rglob('CMakeLists.txt'))  # (ImagingCxx.h)
        for path, names in sorted(self.tree.unresolved.items()):
            for name in sorted(names):
                beside = policy.normal_path(path.rsplit('/', 1)[0] + '/' + name) or ''
                in_vendor = beside.startswith('native/third_party/') and (ROOT / beside).is_file()   # (a test built with a vendored file)
                self.assertTrue(name.endswith('.moc') or name in vendored or in_vendor or '/' + name + '"' in made, (path, name))
        self.assertIn('native/tests/support/testsupport.hpp', self.tree.includes['native/tests/unit/test_json.cpp'])
        self.assertIn('native/tests/gui/gui_support.hpp', self.tree.includes['native/tests/gui/test_gui_canvas.cpp'])

    def test_every_source_of_the_product_is_explained_and_genko_holds_them(self):
        # (a .cpp no header brings would be sent to every test: none today. validation_closure_check.py compares the
        # closures with a real Release build.)
        self.assertEqual([p for p in sorted(self.tree.text) if self.tree.unexplained(p)], [])
        genko = self.tree.closure(self.tree.programs['genko'])
        product = {p for p in self.tree.text if p.startswith('native/src/') and p.endswith(policy.IMPLEMENTATION_SUFFIXES)}
        self.assertGreater(len(product), 600)
        self.assertLessEqual(len(product - genko), 5, sorted(product - genko))

    def test_the_tests_that_run_programs(self):
        runs = self.tree.programs_of()
        for name in ('test_cli_e2e', 'test_ops_e2e', 'test_save_e2e', 'test_app_e2e', 'test_gui_save', 'test_package', 'test_contract_psd', 'test_mcp_api_e2e', 'test_gui_studio'):
            self.assertIn('genko', runs[name], name)
        for name in ('test_json', 'test_model', 'test_fx_lens', 'test_fx', 'test_smart_filters', 'test_raster_ops', 'test_gui_canvas', 'test_storage'):
            self.assertEqual(runs[name], set(), name)
        self.assertIn('genko_lock_helper', runs['test_lock'])
        self.assertEqual({n for n, r in runs.items() if 'genko_fake_mcp' in r}, {'test_gui_ai'})
        self.assertEqual({n for n, r in runs.items() if 'genko_argv_helper' in r}, {'test_mcp_api_e2e'})
        self.assertEqual({n for n, r in runs.items() if 'genko_test_native_plugin' in r}, {'test_contract_plugins', 'test_contract_wasm_plugins', 'test_native_plugins', 'test_wasm_plugins'})
        self.assertEqual(self.tree.carriers['genko'], (['GENKO_CLI', 'genko_cli', 'run_genko'], []))
        count = sum(1 for r in runs.values() if 'genko' in r)
        self.assertTrue(60 <= count <= 110, count)

    def test_known_files_stay_sane(self):
        gui = {n for n in self.everything if n.startswith('test_gui_')}
        heavy = {'test_fx_lens', 'test_fx', 'test_fx_d2', 'test_smart_filters', 'test_smart_stack', 'test_precision_merge_controls'}
        # the app and the drawing files with a family in their names: the rule by names, as it was (not the closure, which
        # would take more: every test that runs genko, every test that draws)
        for path in ('native/src/app/canvas.cpp', 'native/src/app/main_window.hpp', 'native/src/app/CMakeLists.txt'):
            kind, tests, reason = self.reached(path)
            self.assertEqual((kind, tests), ('related', gui | {'test_session', 'test_ops_e2e'}), path)
            self.assertTrue(reason.startswith('名前の規則（2026-11-13 までのまま）: app'), reason)
        kind, tests, reason = self.reached('native/src/render/raster_ops.cpp')
        self.assertTrue(reason.startswith('名前の規則（2026-11-13 までのまま）: render の raster_ops'), reason)
        self.assertLessEqual({'test_raster_ops', 'test_contract_select', 'test_masks', 'test_image', 'test_pynum'}, tests)
        self.assertFalse((heavy | {'test_contract_psd'}) & tests)
        self.assertLess(len(tests), 60)
        self.assertTrue(self.reached('native/src/render/ops_3d.cpp')[2].endswith('render の 3d・mannequin'))
        self.assertTrue(self.reached('native/src/render/ops_tones.cpp')[2].endswith('render の tone・effect・ruler'))
        self.assertEqual(self.reached('native/src/render/text/balloons.cpp')[2], 'ソースの閉包')
        # the service, the formats, the command line: narrower than the drawing code
        for path, inside, outside in (
                ('native/src/service/letter.cpp', {'test_service', 'test_fukidashi_service', 'test_gui_studio', 'test_cli_e2e'}, heavy | {'test_raster_ops'}),
                ('native/src/formats/pdf.cpp', {'test_contract_export', 'test_gui_output', 'test_cli_e2e'}, {'test_fx', 'test_raster_ops', 'test_json'}),
                # (test_gui_ledger_a reads the sources' own text — "/native/src/" — for the words of the errors)
                ('native/src/api/cli.cpp', {'test_cli_e2e', 'test_ops_e2e', 'test_contract_psd', 'test_gui_ledger_a'},
                 heavy | gui - {'test_gui_save', 'test_gui_output', 'test_gui_studio', 'test_gui_ai', 'test_gui_prefs', 'test_gui_ledger_a'}),
                ('native/src/studio/files.cpp', {'test_studio_files', 'test_service'}, heavy)):
            kind, tests, _ = self.reached(path)
            self.assertEqual(kind, 'related', path)
            self.assertLessEqual(inside, tests, path)
            self.assertFalse(outside & tests, (path, sorted(outside & tests)))
        # the drawing code is one piece (the registry of the ops, the page drawn): every test that draws
        kind, tests, _ = self.reached('native/src/render/text/balloons.cpp')
        self.assertLessEqual(heavy | gui | {'test_fukidashi', 'test_render_page'}, tests)
        self.assertFalse({'test_pynum', 'test_lut3d', 'test_prefs_table'} & tests)
        # the model, the readers of the book: all but a handful
        for path in ('native/src/core/model.hpp', 'native/src/storage/reader.cpp', 'native/src/core/command_bus.cpp'):
            kind, tests, _ = self.reached(path)
            self.assertGreaterEqual(len(tests), len(self.everything) - 6, path)
        # a test and what only it includes
        self.assertEqual(self.reached('native/tests/unit/test_strokes.cpp')[1], {'test_strokes'})
        self.assertEqual(self.reached('native/tests/unit/k1_probe.inc')[1], {'test_layout_ideas'})
        self.assertEqual(self.reached('native/tests/perf/test_perf_render.cpp')[1], set())

    def test_data_and_resources_of_the_real_tree(self):
        tree = self.tree
        self.assertEqual(tree.consumers_of_template('native/src/core/ops_schema_data.cpp.in'), {'native/src/core/ops_schema.cpp'})
        self.assertEqual(tree.consumers_of_template('native/src/core/commands_data.cpp.in'), {'native/src/core/commands_list.cpp'})
        self.assertEqual(tree.consumers_of_template('native/src/service/studio_data.cpp.in'), {'native/src/service/data.cpp'})
        self.assertIsNone(tree.consumers_of_template('native/src/core/builtin_profiles_data.cpp.in'))
        for data, template in (('native/src/core/data/ops_schema.json', 'native/src/core/ops_schema_data.cpp.in'),
                               ('native/src/core/data/commands.json', 'native/src/core/commands_data.cpp.in'),
                               ('native/src/core/data/unicode_names.bin', 'native/src/core/pyre_names_data.cpp.in'),
                               ('native/resources/studio/guide/SKILL.md', 'native/src/service/studio_data.cpp.in'),
                               ('native/resources/studio/layouts.json', 'native/src/service/studio_data.cpp.in')):
            self.assertEqual(tree.embedded(data), ({template}, False), data)
        self.assertEqual(tree.embedded('native/resources/genko_spell.qrc'), (set(), False))
        self.assertEqual({n: sorted(tree.resource_users(n)) for n in tree.resources()}, {
            'genko_app': ['native/src/app/icons.cpp'], 'genko_fonts': ['native/src/app/icons.cpp'],
            'genko_materials': ['native/src/app/material_panel.cpp', 'native/src/render/materials.cpp', 'native/src/render/raster_ops.cpp'],
            'genko_spell': ['native/src/service/spell.cpp'], 'genko_text': ['native/src/render/draw.cpp']})
        # the guide an agent reads is built into the service: a change of it is not "a document" (as it was by its .md)
        kind, tests, _ = self.reached('native/resources/studio/guide/SKILL.md')
        self.assertEqual(kind, 'related')
        self.assertLessEqual({'test_service', 'test_ledger_service_b', 'test_mcp_api_e2e', 'test_cli_e2e'}, tests)
        self.assertNotIn('test_fx_lens', tests)
        # the op catalogue is read by nearly everything
        self.assertGreaterEqual(len(self.reached('native/src/core/data/ops_schema.json')[1]), len(self.everything) - 6)
        self.assertEqual(self.reached('native/src/core/data/profiles/sgray.icc')[0], 'broad')
        # files the tests name
        self.assertEqual(self.reached('docs/ops.schema.json')[1], {'test_ledger_service_a'})
        self.assertEqual(self.reached('docs/cpp-migration/THIRD_PARTY.md')[1], {'test_package'})
        self.assertEqual(self.reached('native/tests/e2e/refusal_cases.json')[1], {'test_refusals_e2e'})
        self.assertEqual(self.reached('native/tests/contract/psd_cases.json')[1], {'test_contract_psd'})
        # (review P1-1, live on 2026-11-14: the pages of demo4 are read by test_ledger_service_b through
        # root / "native" / "tests" / "fixtures" / "studio" / "demo4" / ("p00" + n + ".json"))
        for name in ('p001.json', 'p004.json', 'bible.json'):
            kind, tests, _ = self.reached('native/tests/fixtures/studio/demo4/' + name)
            self.assertEqual(kind, 'related', name)
            self.assertLessEqual({'test_ledger_service_b', 'test_ledger_service_a', 'test_mcp_api_e2e', 'test_layout_ideas'}, tests, name)
        # (test_raster_ops says a path ending in "/plugins" of its own scratch folder: who reads data/plugins cannot be told)
        self.assertEqual(self.reached('native/tests/data/plugins/invert.wasm')[0::2], ('broad', '試験の木のデータ（読む試験を言い切れない）'))
        # (the folders the tests get by a macro are not named by a word: GENKO_TEST_DATA, GENKO_SOURCE_DIR, the repository)
        self.assertEqual(self.tree.data_roots(), {'', 'native', 'native/tests/data'})
        self.assertEqual(self.reached('native/tests/data/ruby_cases.json')[1], {'test_ruby', 'test_gui_ruby'})
        self.assertIn('test_fukidashi', self.reached('native/tests/data/fukidashi/old_letters.json')[1])
        self.assertIn('test_ledger_service_b', self.reached('integrations/hermes/genko-manga/SKILL.md')[1])
        self.assertEqual(self.reached('docs/cpp-migration/HANDOFF.md')[0], 'skip')

    def test_the_tests_release_skips_are_always_in_the_asan_run(self):
        # (Release has no fault injection and skips those slots; --pair asks the same slot's PASS of the ASan run of the
        # same plan. At a stage exit ASan runs only what the diff reaches, so these tests must be in the always-run set.)
        skipping = {name for name, sources in self.tree.tests.items() if any('fault injection' in self.tree.text[s] for s in sources)}
        self.assertGreaterEqual(len(skipping), 6)
        self.assertLessEqual(skipping & self.everything, policy.SAFETY)

    def test_a_plan_is_made_in_a_few_seconds(self):
        policy._TREES.clear()
        started = time.monotonic()
        plan = policy.make_plan(['native/src/core/model.hpp', 'native/src/app/canvas.cpp', 'native/src/core/data/ops_schema.json',
                                 'native/tests/data/type/plain_lines.json', 'native/resources/studio/guide/SKILL.md'], 'milestone', ROOT)
        self.assertLess(time.monotonic() - started, 20)
        self.assertEqual(json.dumps(plan, sort_keys=True), json.dumps(policy.make_plan(plan['paths'], 'milestone', ROOT), sort_keys=True))

    def test_the_real_tree_reads_the_same_from_a_windows_checkout(self):
        # (every source, CMake file and .qrc of the real tree written again with CRLF, as git writes them on Windows with
        # core.autocrlf: the same registration, the same closure of every test and program, the same programs run, the
        # same readers of the data; and the changed paths given with backslashes: the same plan)
        with tempfile.TemporaryDirectory() as folder:
            copy = Path(folder)
            for top in policy.CODE_ROOTS + ('native/resources',):
                for file in sorted((ROOT / top).rglob('*')):
                    if file.is_file() and (file.name.endswith(policy.CODE_SUFFIXES + ('.in', '.qrc')) or file.name == 'CMakeLists.txt'):
                        target = copy / file.relative_to(ROOT)
                        target.parent.mkdir(parents=True, exist_ok=True)
                        target.write_bytes(file.read_bytes().replace(b'\r\n', b'\n').replace(b'\n', b'\r\n'))
            other = policy.SourceTree(copy)
            here = self.tree
            self.assertIn(b'\r\n', (copy / 'native/tests/CMakeLists.txt').read_bytes())
            self.assertEqual(sorted(other.text), sorted(here.text))
            self.assertEqual((other.tests, other.programs, other.macros, other.depends), (here.tests, here.programs, here.macros, here.depends))
            self.assertEqual(other.forward, here.forward)
            self.assertEqual(other.unresolved, here.unresolved)
            self.assertEqual(other.programs_of(), here.programs_of())
            self.assertEqual(other.carriers, here.carriers)
            self.assertEqual(other.resources(), here.resources())
            for template in sorted(p for p in here.present if p.endswith('.in')):
                self.assertEqual(other.consumers_of_template(template), here.consumers_of_template(template), template)
            for path in ('native/src/core/data/ops_schema.json', 'native/resources/studio/guide/SKILL.md', 'native/resources/genko_spell.qrc'):
                self.assertEqual(other.embedded(path), here.embedded(path), path)
            for path in ('native/tests/data/type/plain_lines.json', 'native/tests/data/legacy/book-v3.genko/project.json', 'docs/ops.schema.json',
                         'native/src/api/cli.cpp', 'native/tests/fixtures/studio/demo4/bible.json'):
                self.assertEqual(other.tests_naming(path), here.tests_naming(path), path)
            for path in ('native/src/core/model.hpp', 'native/src/service/letter.cpp', 'native/src/render/page.cpp', 'native/tests/support/testsupport.cpp'):
                self.assertEqual(other.tests_reaching(path), here.tests_reaching(path), path)
        paths = ['native/src/service/letter.cpp', 'native/src/app/canvas.cpp', 'native/tests/unit/test_strokes.cpp', 'native/src/core/data/commands.json']
        self.assertEqual(json.dumps(policy.make_plan([p.replace('/', '\\') for p in paths], 'milestone', ROOT), sort_keys=True),
                         json.dumps(policy.make_plan(paths, 'milestone', ROOT), sort_keys=True))

    def test_the_closure_checker_refuses_a_build_it_cannot_judge(self):
        spec = importlib.util.spec_from_file_location('validation_closure_check', ROOT / 'tools/ci/validation_closure_check.py')
        checker = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(checker)
        with tempfile.TemporaryDirectory() as folder:
            build = Path(folder)
            (build / 'compile_commands.json').write_text('[]')
            (build / 'CMakeCache.txt').write_text(f'CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}/native\nGENKO_SANITIZE:STRING=address,undefined\n')
            with self.assertRaisesRegex(ValueError, '共有ライブラリ'):
                checker.check(build)
            (build / 'CMakeCache.txt').write_text(f'CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}/native\nGENKO_SANITIZE:STRING=\nGENKO_SHARED_LIBRARIES:BOOL=ON\n')
            with self.assertRaisesRegex(ValueError, '共有ライブラリ'):
                checker.check(build)
            (build / 'CMakeCache.txt').write_text(f'CMAKE_HOME_DIRECTORY:INTERNAL={ROOT}/native\nGENKO_SANITIZE:STRING=\n')
            (build / 'compile_commands.json').write_text(json.dumps([{'directory': folder, 'command': 'c++ -flto -c a.cpp', 'file': 'a.cpp', 'output': 'a.o'}]))
            with self.assertRaisesRegex(ValueError, 'LTO'):
                checker.check(build)
            # (a build with nothing in it: nothing was compared, so it is not a pass)
            (build / 'compile_commands.json').write_text('[]')
            self.assertFalse(checker.check(build)['passed'])


if __name__ == '__main__':
    unittest.main()
