#!/usr/bin/env python3
"""Build the C++ migration ledger (docs/cpp-migration/ledger.json and LEDGER.md).

Every public contract of the Python baseline gets one row: editing ops, MCP tools and resources, CLI commands,
HTTP routes, export formats, GUI actions, Python modules and test files. Each row names the milestone that
ports it, the C++ module it lands in, the acceptance group that proves it, and its status. Rows whose
milestone cannot be decided make the script exit non-zero (M0's exit needs an empty "unassigned" list).

Static extraction (AST and regular expressions over the fixed baseline). Run from the repository root:
    python3 tools/migration/build_ledger.py [--check]
--check fails when the generated ledger differs from the committed one.
"""
from __future__ import annotations

import ast
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src" / "genko"
DOCS = ROOT / "docs" / "cpp-migration"
BASELINE = "1b7b1d7030d4188d7ebd5e398d010b3ed98b220f"

# --- editing ops ------------------------------------------------------------------------------------------
OPS_M2 = """add_stroke erase erase_raster delete_stroke edit_stroke simplify_stroke set_brush add_layer delete_layer set_layer
set_layers reorder_layers duplicate_layer add_page delete_page duplicate_page reorder select_frame split_frame merge_frame
resize_frame set_frame cut_frame move_gutter add_frame delete_frame name_ok advance set_note set_meta set_autosave lock_page
unlock_page undo""".split()
OPS_M3 = """fill fill_area fill_enclosed fill_gaps flood_fill gradient_fill smudge liquify add_shape vector_edit trace_edit
set_stroke_width reshape_stroke paste store_area forget_area delete_area transform_area put_raster filter_raster
merge_layers merge_visible group_layers move_layers convert_layer merge_down set_layer_mask paint_mask set_paper
define_brush add_tone set_tone delete_tone add_effect edit_effect delete_effect effect_to_layer set_ruler add_ruler
edit_ruler delete_ruler ruler_to_layer ruler_from_3d camera_from_ruler add_prim3d add_scene edit_prim delete_prim
trace_prims add_mannequin pose_mannequin add_figure pose_figure add_head add_hand import_model set_camera set_light
render_prims set_onion step_onion set_lt lt_convert stamp_material set_animation add_anim_folder add_cel set_exposure
set_exposures set_camera_key set_light_table set_timelapse""".split()
OPS_M4 = """add_line edit_line move_line delete_line reorder_lines cut_balloon set_balloon_path set_page_spec set_nombre
set_spread add_cover replace_text for_pages set_assignee import_pages import_psd""".split()
OPS_M5 = """set_bible add_ticket set_ticket add_region adopt_candidate allow_chat_approval approve ask_human attach_reference
bind_ref close_request delete_character delete_location delete_prop delete_region edit_region import_candidates
open_request place_asset propose record_review register_assets reject_sheet reopen_ticket replace_regions
request_approval request_fix resolve_proposal resolve_ticket review_candidates revoke set_candidate set_finish
set_layout set_page_plan set_panel set_placement set_script set_studio set_style_catalog unadopt unbind_ref
upsert_character upsert_location upsert_prop withdraw_candidates""".split()
OP_MODULE = {"M2": "core", "M3": "render", "M4": "text", "M5": "studio"}
OP_ACCEPT = {"M2": ["AC-PARITY", "AC-SAVE"], "M3": ["AC-PARITY", "AC-BRUSH"], "M4": ["AC-PARITY", "AC-TEXT", "AC-EXPORT"],
             "M5": ["AC-PARITY", "AC-API"]}

# --- CLI -------------------------------------------------------------------------------------------------
CLI_MAIN = {"new": "M1", "inspect": "M1", "apply": "M1", "undo": "M1", "redo": "M1", "gc": "M1", "doctor": "M1",
            "schema": "M1", "render": "M3", "export": "M4", "serve": "M5", "token": "M5", "mcp": "M5", "studio": "M5",
            "app": "M2", "register-links": "M6"}
EXPORT_FORMATS = {"png": "M4", "tiff": "M4", "pdf": "M4", "strip": "M4", "psd": "M4", "epub": "M4", "pack": "M4",
                  "webtoon": "M4", "sns": "M4", "cmyk": "M4", "layers": "M4", "kindle": "M4", "timelapse": "M4",
                  "animation": "M4"}

# --- Python modules ---------------------------------------------------------------------------------------
MODULES = {
    "M1": ("core/storage", """__init__ __main__ assets backup blobcache headless io journal lock maintenance migrate models
            ops pipeline"""),
    "M2": ("core/render/app", """frames stroke render app/__init__ app/main app/canvas app/live_ink app/session app/documents
            app/theme app/icons app/fields app/inputs app/dialogs app/pages_panel app/navigator app/history app/viewer
            app/wording"""),
    "M3": ("render", """abr anim animops brushes effects fill filters layerops lt mannequin materials/__init__ materials/builtin
            mesh3d persp3d placement plugins poses prim3d raster rulers selection selops threeops timelapse tones
            vectorize warp fileops app/brush_panel app/canvas_guides app/canvas_shapes app/canvas_vector app/colours
            app/filter_dialog app/gradient_editor app/guide_panel app/timeline app/subview app/tool_settings
            app/actions app/material_panel"""),
    "M4": ("text/formats", """balloons bookops checks colour covers export fonts merge nombre pack pagespec profiles psd
            scanner screentone tategaki app/lettering app/text_style app/story_editor app/exporting app/printing
            app/bookview app/check_panel"""),
    "M5": ("studio/api", """guide lineart server stylecat tokens upscale mcp/__init__ mcp/server studio/__init__ studio/adopt
            studio/atari studio/blocking studio/claims studio/cli studio/drafts studio/evaluate studio/finish
            studio/fxwords studio/genreq studio/importer studio/issues studio/jobs studio/jsonschema_lite
            studio/jsonutil studio/layout studio/letter studio/lint studio/preflight studio/presence studio/review
            studio/schemas studio/service studio/state studio/studio_ops studio/toollog studio/tools_registry
            studio/worklist studio/xycut app/ai_link app/studio_widgets app/review_model app/style_picker"""),
    "M6": ("app/packaging", """app/help app/preferences app/workspace app/comfort app/glass app/dialog_look app/links"""),
}

# --- GUI actions: keywords in the attribute name → milestone ------------------------------------------------
GUI_RULES = [
    ("M5", r"(ai|chat_approval|style|review|approve|studio|link)"),
    ("M6", r"(prefs|help|about|faq|keys|guide$|find_command|workspace|commandbar|quick|hints|tool_names|comfort)"),
    ("M4", r"(line|balloon|story|replace|export|print|nombre|cover|book_preview|checks|text|paper|spread|assignee|"
           r"merge_book|import_psd|cmyk|phone|scale)"),
    ("M3", r"(fill|gradient|blend|shape|vector|reshape|liquify|marquee|lasso|wand|sel_|ruler|3d|effect|stamp|tone|"
           r"materials|figure|stick|head|hand|box|cylinder|sphere|cone|stairs|floor|obj|trace|prim|persp|grid|snap|"
           r"warp|flip|transform|pivot|layer_|plugins|onion|timeline|timelapse|scan|scanner|picker|swap_colour|"
           r"transparent|thicker|thinner|line_width|quick_mask|cut|copy|paste|delete_area|select_all|deselect|"
           r"actions|camera|horizon|screen_dots|import)"),
    ("M2", r".*"),
]

TEST_RULES = [  # file name → how its checks move to C++ (port | contract | replace) and the milestone
    ("M2", "replace", r"test_(m7_gui|m10|ui_look|ux|ux_review|w1_panels_gui|w9_view|m7_session|w14_speed)"),
    ("M5", "contract", r"test_(studio|agent|ai_|chat_approval|battle|m4_pipeline|m3_art|m3_report|m1_report|"
                       r"m2_group|m5|m6|m8|m9|m11|m12|m13|m14|m15|m16|p7|p9|tibo|t_report|hd_report|quality|"
                       r"server|m1_security|l1_style)"),
    ("M4", "contract", r"test_(b_lettering|w6|tategaki|story_and_export|p5_psd|p5_pack|p5_balloon|j9|j10|f4|f6|"
                       r"w15_color|m2_format|m4_units|j11|w13_scan|p5_spread|effect_clearing)"),
    ("M3", "contract", r"test_(w2|w3|w4|w5|w7|w8|w10|w11|w12|j1|j2|j3|j4|j5|j6|j7|j8|j12|p5_|p4|p6|p8|f1|f2|f3|f5|"
                       r"g\d|h2|h4|h5|k1|c_extras|render_clip|i0)"),
    ("M2", "contract", r"test_w1_panels$"),
    ("M1", "port", r"test_(io|lock|migrate|ops|headless|project|cli|p1|p2|p3|frames|h1)"),
]


def _words(text: str) -> list[str]:
    return text.split()


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def ops_rows(schema_names: list[str]) -> tuple[list[dict], list[str]]:
    rows, unassigned = [], []
    groups = {"M2": OPS_M2, "M3": OPS_M3, "M4": OPS_M4, "M5": OPS_M5}
    seen: dict[str, str] = {}
    for ms, names in groups.items():
        for name in names:
            if name in seen:
                raise SystemExit(f"op {name} is in both {seen[name]} and {ms}")
            seen[name] = ms
    for name in schema_names:
        ms = seen.get(name)
        if ms is None:
            unassigned.append(f"op:{name}")
            continue
        rows.append({"id": f"op:{name}", "kind": "op", "name": name, "milestone": ms, "cpp_module": OP_MODULE[ms],
                     "acceptance": OP_ACCEPT[ms], "checks": ["success", "invalid_input_no_change", "permission",
                                                             "undo", "save_reopen", "gui_or_api_entry"],
                     "status": "pending"})
    extra = sorted(set(seen) - set(schema_names))
    if extra:
        unassigned += [f"op-not-in-schema:{n}" for n in extra]
    return rows, unassigned


def mcp_rows(tools: list[dict], resources: list[dict]) -> list[dict]:
    out = [{"id": f"mcp-tool:{t['name']}", "kind": "mcp_tool", "name": t["name"], "signature": t.get("signature", ""),
            "milestone": "M5", "cpp_module": "api", "acceptance": ["AC-API"], "status": "pending"} for t in tools]
    out += [{"id": f"mcp-resource:{r['uri']}", "kind": "mcp_resource", "name": r["uri"], "milestone": "M5",
             "cpp_module": "api", "acceptance": ["AC-API"], "status": "pending"} for r in resources]
    return out


def cli_rows(literal: dict) -> tuple[list[dict], list[str]]:
    rows, unassigned = [], []
    for cmd in literal.get("src/genko/__main__.py", []):
        ms = CLI_MAIN.get(cmd)
        if ms is None:
            unassigned.append(f"cli:{cmd}")
            continue
        rows.append({"id": f"cli:{cmd}", "kind": "cli", "name": f"genko {cmd}", "milestone": ms, "cpp_module": "api",
                     "acceptance": ["AC-API"], "status": "pending"})
    for cmd in literal.get("src/genko/studio/cli.py", []):
        rows.append({"id": f"cli:studio {cmd}", "kind": "cli", "name": f"genko studio {cmd}", "milestone": "M5",
                     "cpp_module": "api", "acceptance": ["AC-API"], "status": "pending"})
    return rows, unassigned


def http_rows() -> list[dict]:
    text = (SRC / "server.py").read_text(encoding="utf-8")
    exact = sorted(set(re.findall(r'route == "([^"]+)"', text)))
    prefix = sorted(set(re.findall(r'route\.startswith\("([^"]+)"', text)))
    return [{"id": f"http:{r}", "kind": "http", "name": r, "milestone": "M5", "cpp_module": "api",
             "acceptance": ["AC-API"], "status": "pending"} for r in exact + [p + "*" for p in prefix]]


def export_rows(choices: dict) -> list[dict]:
    names = choices.get("src/genko/__main__.py", [])
    return [{"id": f"export:{n}", "kind": "export_format", "name": n, "milestone": EXPORT_FORMATS[n],
             "cpp_module": "formats", "acceptance": ["AC-EXPORT"], "status": "pending"} for n in names]


def gui_rows() -> list[dict]:
    rows = []
    seen = set()
    for path in sorted((SRC / "app").glob("*.py")):
        text = path.read_text(encoding="utf-8")
        for attr, label1, label2 in re.findall(r'self\.(act_\w+)\s*=\s*a\(\s*(?:f?"([^"]*)"|\'([^\']*)\')', text):
            if attr in seen:
                continue
            seen.add(attr)
            key = attr[4:]
            ms = next(m for m, pattern in GUI_RULES if re.search(pattern, key))
            rows.append({"id": f"gui:{attr}", "kind": "gui_action", "name": label1 or label2, "attribute": attr,
                         "source": str(path.relative_to(ROOT)), "milestone": ms, "cpp_module": "app",
                         "acceptance": ["AC-PARITY", "AC-UX"], "status": "pending"})
    for group in ("stage_actions", "tool_actions", "ruler_actions", "prop_actions", "scene_actions", "effect_actions",
                  "pose_actions", "border_kind_actions", "launcher_actions"):
        ms = {"stage_actions": "M2", "tool_actions": "M2", "launcher_actions": "M6", "border_kind_actions": "M2"}.get(group, "M3")
        rows.append({"id": f"gui-group:{group}", "kind": "gui_action_group", "name": group, "milestone": ms,
                     "cpp_module": "app", "acceptance": ["AC-PARITY", "AC-UX"], "status": "pending"})
    return rows


def module_rows(modules: list[dict]) -> tuple[list[dict], list[str]]:
    lookup = {}
    for ms, (target, names) in MODULES.items():
        for name in _words(names):
            lookup[name] = (ms, target)
    rows, unassigned = [], []
    for item in modules:
        rel = item["path"].removeprefix("src/genko/").removesuffix(".py")
        if rel not in lookup:
            unassigned.append(f"module:{rel}")
            continue
        ms, target = lookup[rel]
        rows.append({"id": f"module:{rel}", "kind": "python_module", "name": item["path"], "sha256": item["sha256"],
                     "milestone": ms, "cpp_module": target, "acceptance": ["AC-PARITY"], "status": "pending"})
    missing = sorted(set(lookup) - {m["path"].removeprefix("src/genko/").removesuffix(".py") for m in modules})
    unassigned += [f"module-not-found:{m}" for m in missing]
    return rows, unassigned


def test_rows() -> tuple[list[dict], list[str]]:
    rows, unassigned = [], []
    for path in sorted((ROOT / "tests").glob("test_*.py")):
        name = path.stem
        tree = ast.parse(path.read_text(encoding="utf-8"))
        tests = [n.name for n in ast.walk(tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
                 and n.name.startswith("test_")]
        hit = next(((ms, how) for ms, how, pattern in TEST_RULES if re.match(pattern, name)), None)
        if hit is None:
            unassigned.append(f"test:{name}")
            continue
        ms, how = hit
        rows.append({"id": f"test:{name}", "kind": "test_file", "name": str(path.relative_to(ROOT)),
                     "tests": tests, "milestone": ms, "how": how, "acceptance": ["AC-PARITY"], "status": "pending"})
    return rows, unassigned


# --- the C++ side (M7: every row counted against the C++ tree) -----------------------------------------------
# A row is "ported" when the C++ source has it and a C++ test (or a case the contract tests run on both builds) names
# it; "in_progress" when only part of it is there (what is missing is listed); "not_applicable" for Python-only
# machinery, with the reason; "pending" when nothing is found. "accepted" only from a passing --pair of every
# configuration the plan names (`--accepted <validation folder>`, Windows included), never by hand.
NATIVE = ROOT / "native"
TEST_TREES = (NATIVE / "tests", ROOT / "tools" / "migration", ROOT / "tools" / "acceptance")
BUS_OPS = ("undo", "for_pages")  # (taken by the command bus itself, not the op registry: core/command_bus.cpp)
GROUP_TESTS = {  # the groups the window keeps without a name of their own: a member found by its words in a test
    "ruler_actions": r'titled\(QStringLiteral\("[^"]*定規"\)\)->trigger', "prop_actions": r'titled\(QStringLiteral\("小物: ',
    "scene_actions": r'titled\(QStringLiteral\("背景: ', "pose_actions": r'titled\(QStringLiteral\("ポーズ: ',
    "effect_actions": r'"cmd:%1"\)\.arg',
}

# Python modules and test files: the C++ code and tests that carry them (read and judged for M7). Every file named
# must exist, and a C++ test file must be registered in native/tests/CMakeLists.txt. verdict: ported (covered) |
# partial (with what is missing) | not_applicable (with the reason).
MODULE_CPP: dict[str, dict] = {
 "src/genko/__init__.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/version.cpp",
   "native/src/core/version.hpp",
   "native/src/api/buildinfo.cpp"
  ],
  "tests": [
   "native/tests/unit/test_buildinfo.cpp",
   "native/tests/package/test_package.cpp"
  ],
  "note": "Only __version__ = \"0.1.0\"; genko::version() returns the same string and `genko --version` reports it."
 },
 "src/genko/__main__.py": {
  "verdict": "partial",
  "cpp": [
   "native/src/api/cli.cpp",
   "native/src/api/main.cpp",
   "native/src/api/server_cli.cpp",
   "native/src/api/studio_cli.cpp",
   "native/src/app/links.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_cli.cpp",
   "native/tests/e2e/test_cli_e2e.cpp",
   "native/tests/contract/test_contract_ops_cli.cpp",
   "native/tests/contract/test_contract_render_cli.cpp"
  ],
  "missing": [
   "`genko render --out` in a format Pillow writes and this build does not (GIF, ICO, PPM, TGA, JPEG 2000 ...): NotYetPorted format:<ext>; PNG, JPEG, TIFF, PDF, WebP, BMP and DIB are written"
  ],
  "note": "Every argparse subcommand (new, export, inspect, apply, render, schema, serve, undo, redo, gc, doctor, token, app, register-links, studio, mcp) is dispatched by api/cli.cpp / api/main.cpp."
 },
 "src/genko/abr.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/abr.cpp",
   "native/src/render/abr.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_abr.cpp",
   "native/tests/gui/test_gui_brushes.cpp"
  ],
  "note": "Photoshop .abr reading (v1/2 and 6/7/10 8BIM samp, 8-bit sampled tips) with extra size bounds."
 },
 "src/genko/anim.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/anim.cpp",
   "native/src/render/anim.cpp",
   "native/src/render/movie.cpp",
   "native/src/render/gif.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_anim.cpp",
   "native/tests/gui/test_gui_anim.cpp",
   "native/tests/contract/test_contract_export_cli.cpp"
  ],
  "note": "Timeline model, onion skin, frame rendering and GIF/WebP/APNG/MP4/PNG-sequence export."
 },
 "src/genko/animops.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/ops_anim.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_anim.cpp"
  ],
  "note": "Animation ops compared against Python's apply_ops."
 },
 "src/genko/app/__init__.py": {
  "verdict": "not_applicable",
  "note": "Package docstring only (\"Desktop UI.\"); nothing to port."
 },
 "src/genko/app/actions.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/actions.cpp",
   "native/src/app/actions.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_carried.cpp"
  ],
  "note": "オートアクション: record, manage and play back ops."
 },
 "src/genko/app/ai_link.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/ai_link.cpp",
   "native/src/app/ai_dialog.cpp",
   "native/src/app/client_config.cpp",
   "native/src/app/mcp_probe.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_ai.cpp",
   "native/tests/contract/test_contract_studio_gui.cpp"
  ],
  "note": "AI と作る dialog, client settings and connection states."
 },
 "src/genko/app/bookview.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/bookview.cpp",
   "native/src/app/bookview.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_carried.cpp",
   "native/tests/gui/test_gui_lines.cpp"
  ],
  "note": "Book-form preview and ReplaceDialog (台詞の検索・置換)."
 },
 "src/genko/app/brush_panel.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/brush_panel.cpp",
   "native/src/app/brush_panel.hpp",
   "native/src/app/main_window_brushes.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_brushes.cpp",
   "native/tests/gui/test_gui_paper.cpp"
  ],
  "note": "Brush panel, brush dialog, fill/eraser settings ported; only the WebP tip picture is left out. M7: a brush tip from a WebP picture is offered and read (render/tiff_webp)"
 },
 "src/genko/app/canvas.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/canvas.cpp",
   "native/src/app/canvas.hpp",
   "native/src/app/canvas_input.cpp",
   "native/src/app/canvas_paint.cpp",
   "native/src/app/canvas_lines.cpp",
   "native/src/app/canvas_select.cpp",
   "native/src/app/canvas_tools.cpp",
   "native/src/app/canvas_gpu.cpp",
   "native/src/app/tiles.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_canvas.cpp",
   "native/tests/gui/test_gui_pen.cpp",
   "native/tests/gui/test_gui_select.cpp"
  ],
  "note": "PageCanvas split over canvas_*.cpp (input, painting, panel tool, text/balloon handles, selection, view)."
 },
 "src/genko/app/canvas_guides.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/canvas_guides.cpp",
   "native/src/app/canvas_effects.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_guides.cpp"
  ],
  "note": "Rulers, grid and 3D handles on the canvas."
 },
 "src/genko/app/canvas_shapes.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/canvas_tools.cpp",
   "native/src/app/canvas_select.cpp",
   "native/src/app/canvas_scale.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_paint.cpp",
   "native/tests/gui/test_gui_select.cpp",
   "native/tests/gui/test_gui_output.cpp"
  ],
  "note": "Figures, selection shapes and the scales with pulled-out guides."
 },
 "src/genko/app/canvas_vector.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/canvas_vector.cpp",
   "native/src/app/main_window_vector.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_paint.cpp",
   "native/tests/gui/test_gui_actions.cpp"
  ],
  "note": "The vector line-edit tool."
 },
 "src/genko/app/check_panel.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/check_panel.cpp",
   "native/src/app/check_panel.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_output.cpp"
  ],
  "note": "入稿前の点検 panel."
 },
 "src/genko/app/colours.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/colours.cpp",
   "native/src/app/colours.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_colours.cpp"
  ],
  "note": "Colour panel (square, hue bar, sets, recent, mixing)."
 },
 "src/genko/app/comfort.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/comfort.cpp",
   "native/src/app/comfort.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_look.cpp"
  ],
  "note": "Page-only view, stage layouts, round menu, rest reminder; the macOS reduce-motion query is not ported (macOS is outside the migration's scope, SPEC.md)."
 },
 "src/genko/app/dialog_look.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/look.cpp",
   "native/src/app/look.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_look.cpp"
  ],
  "note": "Shared dialog parts (header, sections, card, footer)."
 },
 "src/genko/app/dialogs.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/dialogs.cpp",
   "native/src/app/dialogs.hpp",
   "native/src/app/export_dialog.cpp",
   "native/src/app/timeline.cpp",
   "native/src/app/templates.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_book.cpp",
   "native/tests/gui/test_gui_output.cpp",
   "native/tests/gui/test_gui_layout.cpp"
  ],
  "note": "Paper/New/Start/Export/Timelapse/Template dialogs exist; the C++ app applies templates with panel ops instead of set_layout and refuses three cases Python handles. M7: TemplateDialog on a page with lines or drawn panels clears it first as layout.clear_page (app/templates.cpp; test_gui_actions theTemplates)"
 },
 "src/genko/app/documents.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/documents.hpp",
   "native/src/app/main_window.cpp",
   "native/src/app/session.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_actions.cpp"
  ],
  "note": "Several books as tabs and one book in several windows (shared Session replaces notify())."
 },
 "src/genko/app/exporting.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/formats/exporting.cpp",
   "native/src/service/human_service.cpp",
   "native/src/app/export_dialog.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/gui/test_gui_output.cpp"
  ],
  "note": "The 12 formats and run(); the official export goes through HumanService (formats::run alone answers not_yet_ported without that hook)."
 },
 "src/genko/app/fields.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/fields.cpp",
   "native/src/app/fields.hpp",
   "native/src/app/material_tabs.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_brushes.cpp",
   "native/tests/gui/test_gui_effects.cpp"
  ],
  "note": "slider_for/with_value/LineSample in fields.cpp, effect_picture/effect_tiles in material_tabs.cpp; tested only through the panels that use them."
 },
 "src/genko/app/filter_dialog.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/layer_dialogs.cpp",
   "native/src/app/layer_dialogs.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_layers.cpp"
  ],
  "note": "CurveEditor, histogram and the filter dialog with live preview (filter_params)."
 },
 "src/genko/app/glass.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/glass.cpp",
   "native/src/app/glass.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_look.cpp"
  ],
  "note": "Windows 11 Acrylic for floating parts; Python's macOS vibrancy is not ported (no macOS target, SPEC.md)."
 },
 "src/genko/app/gradient_editor.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/layer_dialogs.cpp",
   "native/src/app/layer_dialogs.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_layers.cpp"
  ],
  "note": "StopsEditor with ready-made rows and stops_from()."
 },
 "src/genko/app/guide_panel.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/guide_panel.cpp",
   "native/src/app/guide_panel.hpp",
   "native/src/app/main_window_guides.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_guides.cpp"
  ],
  "note": "The rulers/3D panel."
 },
 "src/genko/app/help.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/help.cpp",
   "native/src/app/help.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_layout.cpp",
   "native/tests/gui/test_gui_carried.cpp"
  ],
  "note": "Guide, FAQ, shortcut list and about; the text now names menus through {menu:act_x} references."
 },
 "src/genko/app/history.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/history_panel.cpp",
   "native/src/app/history_panel.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_output.cpp"
  ],
  "note": "describe(), the timeline and the 履歴 panel (click to undo/redo to a point)."
 },
 "src/genko/app/icons.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/icons.cpp",
   "native/src/app/icons.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_look.cpp"
  ],
  "note": "Tool icons (Lucide and drawn) in the look's colours."
 },
 "src/genko/app/inputs.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/inputs.cpp",
   "native/src/app/inputs.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_look.cpp"
  ],
  "note": "The number-field/wheel watcher (install, watch)."
 },
 "src/genko/app/lettering.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/lettering.cpp",
   "native/src/app/lettering.hpp",
   "native/src/render/text/lettering.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/contract/test_contract_lines.cpp"
  ],
  "note": "描き文字 and line lettering in the window."
 },
 "src/genko/app/links.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/links.cpp",
   "native/src/app/links.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_studio.cpp",
   "native/tests/contract/test_contract_studio_gui.cpp"
  ],
  "note": "genko:// links, single-instance hand-over, register-links."
 },
 "src/genko/app/live_ink.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/live_ink.cpp",
   "native/src/app/live_ink.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_pen.cpp",
   "native/tests/gui/test_gui_guides.cpp"
  ],
  "note": "The live line drawn while the pen moves, checked pixel for pixel against the committed line."
 },
 "src/genko/app/main.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/layer_panel.cpp",
   "native/src/app/main_window.cpp",
   "native/src/app/main_window_actions.cpp",
   "native/src/app/main_window_book.cpp",
   "native/src/app/main_window_lines.cpp",
   "native/src/app/main_window_materials.cpp",
   "native/src/app/main_window_paint.cpp",
   "native/src/app/main_window_studio.cpp",
   "native/src/app/story_panel.cpp",
   "native/src/app/tool_settings.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_carried.cpp",
   "native/tests/gui/test_gui_save.cpp",
   "native/tests/e2e/test_app_e2e.cpp"
  ],
  "note": "All 202 QActions with Python's names and titles exist in main_window*.cpp; only the picture formats above are missing. M7: TIFFs in planes are read as Pillow 12 reads them (render/tiff_webp.cpp; test_contract_tiff_webp); the side panels fit their width (app::fit_narrow, as tool_settings.fit_narrow)"
 },
 "src/genko/app/material_panel.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/material_tabs.cpp",
   "native/src/app/material_panel.cpp",
   "native/src/app/material_preview_cache.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_materials.cpp",
   "native/tests/gui/test_gui_effects.cpp",
   "native/tests/unit/test_material_preview_cache.cpp"
  ],
  "note": "Materials, tone settings and effect tabs (picture import limits are those of materials/__init__)."
 },
 "src/genko/app/navigator.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/navigator.cpp",
   "native/src/app/navigator.hpp",
   "native/src/app/thumbs.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_actions.cpp"
  ],
  "note": "Navigator and the page overview."
 },
 "src/genko/app/pages_panel.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/pages_panel.cpp",
   "native/src/app/pages_panel.hpp",
   "native/src/app/book_dialogs.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_book.cpp",
   "native/tests/gui/test_gui_actions.cpp"
  ],
  "note": "The page list with its menu (spreads, moves, covers, assignee)."
 },
 "src/genko/app/preferences.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/preferences.cpp",
   "native/src/app/preferences.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_look.cpp",
   "native/tests/gui/test_gui_workspace.cpp"
  ],
  "note": "環境設定 (shortcuts, tablet, look, comfort, GPU canvas)."
 },
 "src/genko/app/printing.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/print_dialog.cpp",
   "native/src/app/print_dialog.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_output.cpp"
  ],
  "note": "QPrinter/QPrintDialog printing at the printer's resolution."
 },
 "src/genko/app/review_model.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/review_model.cpp",
   "native/src/app/review_model.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_studio_gui.cpp",
   "native/tests/gui/test_gui_studio.cpp"
  ],
  "note": "Approval box model compared against Python."
 },
 "src/genko/app/session.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/session.cpp",
   "native/src/app/session.hpp",
   "native/src/app/save_status.cpp"
  ],
  "tests": [
   "native/tests/unit/test_session.cpp",
   "native/tests/gui/test_gui_save.cpp"
  ],
  "note": "Editing session (apply, undo in memory, commit, autosave, outside changes via check_outside)."
 },
 "src/genko/app/story_editor.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/story_editor.cpp",
   "native/src/app/story_editor.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_ruby.cpp"
  ],
  "note": "Story editor."
 },
 "src/genko/app/studio_widgets.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/studio_widgets.cpp",
   "native/src/app/studio_widgets.hpp",
   "native/src/app/reply_box.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_studio.cpp",
   "native/tests/gui/test_gui_ai.cpp"
  ],
  "note": "Approval box, process bar, panel view, library and chat approvals."
 },
 "src/genko/app/style_picker.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/style_picker.cpp",
   "native/src/app/style_picker.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_studio.cpp"
  ],
  "note": "絵柄を選ぶ."
 },
 "src/genko/app/subview.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/subview.cpp",
   "native/src/app/subview.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_effects.cpp"
  ],
  "note": "Reference pictures beside the page (read with QImage, as Python)."
 },
 "src/genko/app/text_style.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/text_style.cpp",
   "native/src/app/text_style.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_lines.cpp"
  ],
  "note": "Text style helpers."
 },
 "src/genko/app/theme.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/theme.cpp",
   "native/src/app/theme.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_look.cpp",
   "native/tests/gui/test_gui_layout.cpp"
  ],
  "note": "Tokens, light/dark/system modes, palette, style sheet, UI font choice."
 },
 "src/genko/app/timeline.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/timeline.cpp",
   "native/src/app/timeline.hpp",
   "native/src/app/main_window_anim.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_anim.cpp"
  ],
  "note": "Timeline panel and the timelapse dialog."
 },
 "src/genko/app/tool_settings.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/tool_settings.cpp",
   "native/src/app/tool_settings.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_effects.cpp",
   "native/tests/gui/test_gui_brushes.cpp",
   "native/tests/gui/test_gui_lines.cpp"
  ],
  "note": "ツールの設定 pages per tool."
 },
 "src/genko/app/viewer.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/viewer.cpp",
   "native/src/app/viewer.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_studio.cpp"
  ],
  "note": "ImageView and ViewerDialog."
 },
 "src/genko/app/wording.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/wording.cpp",
   "native/src/service/wording.hpp"
  ],
  "tests": [
   "native/tests/unit/test_session.cpp",
   "native/tests/gui/test_gui_output.cpp"
  ],
  "note": "English error to Japanese rules (regex table) moved into the service library."
 },
 "src/genko/app/workspace.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/workspace.cpp",
   "native/src/app/workspace.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_workspace.cpp"
  ],
  "note": "Named workspaces, quick access, command bar, command search, cursors."
 },
 "src/genko/assets.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/storage/asset_store.cpp",
   "native/src/storage/asset_store.hpp"
  ],
  "tests": [
   "native/tests/unit/test_storage.cpp",
   "native/tests/contract/test_contract_storage.cpp"
  ],
  "note": "AssetStore (ref, relpath, path, put_bytes, get_bytes, all_files) with stricter ref/suffix validation."
 },
 "src/genko/backup.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/backup.cpp",
   "native/src/app/backup.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_look.cpp"
  ],
  "note": "backups/make/due ported (zip64 too) and tested in test_gui_look's backup cases."
 },
 "src/genko/balloons.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/text/balloons.cpp",
   "native/src/render/text/balloons.hpp",
   "native/src/core/ops_lines.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_lines.cpp"
  ],
  "note": "Balloon shapes, tails, joins and draw_lines byte for byte."
 },
 "src/genko/blobcache.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/storage/reader.cpp",
   "native/src/storage/writer.cpp",
   "native/src/core/model.hpp"
  ],
  "tests": [
   "native/tests/unit/test_storage.cpp",
   "native/tests/unit/test_strokes.cpp",
   "native/tests/contract/test_contract_storage.cpp"
  ],
  "note": "Redesigned: stroke lists carry their blob_ref (writer reuses it) and LoadCache shares decoded blobs between loads instead of a global id-keyed LRU."
 },
 "src/genko/bookops.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/ops_bookpages.cpp",
   "native/src/core/command_bus.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_book.cpp"
  ],
  "note": "add_cover, replace_text, set_assignee and for_pages expansion."
 },
 "src/genko/brushes.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/brushes.cpp",
   "native/src/render/brushes.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_brushes.cpp",
   "native/tests/gui/test_gui_brushes.cpp"
  ],
  "note": "Presets, custom brushes (tips, stamps, scatter, textures) and their drawing."
 },
 "src/genko/checks.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/formats/checks.cpp",
   "native/src/formats/checks.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_checks.cpp",
   "native/tests/gui/test_gui_output.cpp"
  ],
  "note": "入稿前の点検 report compared byte for byte."
 },
 "src/genko/colour.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/colour.cpp",
   "native/src/render/colour.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_colour.cpp"
  ],
  "note": "CMYK conversion, ICC and screen proof."
 },
 "src/genko/covers.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/covers.cpp",
   "native/src/render/covers.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_book.cpp",
   "native/tests/gui/test_gui_book.cpp"
  ],
  "note": "Cover kinds, specs, folds and reading order."
 },
 "src/genko/effects.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/effects.cpp",
   "native/src/render/effects.hpp",
   "native/src/render/ops_effects.cpp"
  ],
  "tests": [
   "native/tests/unit/test_tones_effects.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/test_contract_tone_render.cpp"
  ],
  "note": "Effect lines (focus, speed, uni/beta flash, white) bit-for-bit geometry."
 },
 "src/genko/export.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/formats/export.cpp",
   "native/src/formats/pdf.cpp",
   "native/src/formats/pillow_save.cpp",
   "native/src/formats/output.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_export_cli.cpp"
  ],
  "note": "PNG/TIFF/PDF/strip/EPUB/Kindle/CMYK exports, files byte for byte."
 },
 "src/genko/fileops.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/import_psd.cpp",
   "native/src/core/ops_arrange.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_psd.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/gui/test_gui_book.cpp"
  ],
  "note": "import_psd (+placed) and set_timelapse ops."
 },
 "src/genko/fill.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/fill.cpp",
   "native/src/render/fill_patches.cpp"
  ],
  "tests": [
   "native/tests/unit/test_raster_ops.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp"
  ],
  "note": "Region fill with gap closing, mask and polygon patches."
 },
 "src/genko/filters.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/filters.cpp",
   "native/src/core/filters.cpp",
   "native/src/render/color_filters.cpp",
   "native/src/render/composite.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_filters.cpp",
   "native/tests/unit/test_color_filters.cpp"
  ],
  "note": "All 22 KINDS and the 9 ADJUSTMENTS; plugin:<name> goes to the plugin runner."
 },
 "src/genko/fonts.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/text/fonts.cpp",
   "native/src/render/text/fonts.hpp",
   "native/src/render/draw.cpp",
   "native/src/app/story_panel.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/gui/test_gui_lines.cpp"
  ],
  "note": "Faces, glyph checks, kana handling and system_fonts ported. M7: an unreadable font file draws with Pillow's load_default() at size 10 (text::Fonts::truetype); files up to 256 MB are read"
 },
 "src/genko/frames.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/frames.cpp",
   "native/src/core/frames.hpp",
   "native/src/app/frame_tools.cpp",
   "native/src/render/panels.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/unit/test_ops.cpp",
   "native/tests/gui/test_gui_canvas.cpp"
  ],
  "note": "Panel geometry (shape, outline, offset, cut, relayout, move_gutter, reading_order) in core/frames; gutters() in app/frame_tools."
 },
 "src/genko/guide.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/guide.cpp",
   "native/src/service/guide.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/unit/test_service.cpp"
  ],
  "note": "Composition, pose, keepout, mask and compare guides for generation requests."
 },
 "src/genko/headless.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/storage/snapshot.cpp",
   "native/src/storage/snapshot.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_cli.cpp",
   "native/tests/contract/test_contract_save.cpp",
   "native/tests/e2e/test_cli_e2e.cpp"
  ],
  "note": "snapshot() and inspect_stroke() behind `genko inspect`, compared with Python's output."
 },
 "src/genko/io.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/storage/reader.cpp",
   "native/src/storage/writer.cpp",
   "native/src/storage/transaction.cpp",
   "native/src/storage/fsutil.cpp"
  ],
  "tests": [
   "native/tests/unit/test_storage.cpp",
   "native/tests/unit/test_transaction.cpp",
   "native/tests/contract/test_contract_save.cpp",
   "native/tests/contract/test_contract_storage.cpp"
  ],
  "note": "load_episode/save_episode/gate_changes ported; the writer writes schema v4 (journal transactions) where Python wrote v3."
 },
 "src/genko/journal.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/storage/journal.cpp",
   "native/src/storage/undo.cpp",
   "native/src/storage/gc.cpp"
  ],
  "tests": [
   "native/tests/unit/test_journal.cpp",
   "native/tests/unit/test_transaction.cpp",
   "native/tests/e2e/test_save_e2e.cpp"
  ],
  "note": "append/audit/entries/stacks/restore/referenced_assets/refs_in ported onto the v4 transaction journal."
 },
 "src/genko/layerops.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/raster_ops.cpp",
   "native/src/core/ops_arrange.cpp",
   "native/src/render/ops_paint.cpp"
  ],
  "tests": [
   "native/tests/unit/test_raster_ops.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/test_contract_paint.cpp"
  ],
  "note": "merge/merge_visible/group/move/convert/set_layers/set_paper/liquify."
 },
 "src/genko/lineart.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/filters.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_filters.cpp",
   "native/tests/contract/test_contract_service.cpp"
  ],
  "note": "LineParams and extract (the lineart filter and the service's line extraction)."
 },
 "src/genko/lock.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/storage/lock.cpp",
   "native/src/storage/lock.hpp"
  ],
  "tests": [
   "native/tests/unit/test_lock.cpp"
  ],
  "note": "ProjectLock (OS lock plus holder info) with a cross-process helper in the tests."
 },
 "src/genko/lt.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/raster_ops.cpp"
  ],
  "tests": [
   "native/tests/unit/test_raster_ops.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp"
  ],
  "note": "to_line_art and runs_to_strokes inlined in the lt_convert op."
 },
 "src/genko/maintenance.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/storage/gc.cpp",
   "native/src/storage/doctor.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_save.cpp",
   "native/tests/e2e/test_save_e2e.cpp",
   "native/tests/unit/test_transaction.cpp"
  ],
  "note": "gc() and the asset/path checks of doctor() are ported; the font check is still skipped with a stale M4 reason (the C++ fonts are compiled in, so it may only need retiring). M7: doctor checks the bundled font (api/cli.cpp doctor)"
 },
 "src/genko/mannequin.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/mannequin.cpp",
   "native/src/core/mannequin.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/unit/test_ops_3d.cpp"
  ],
  "note": "Posable figure skeleton and its lines."
 },
 "src/genko/materials/__init__.py": {
  "verdict": "partial",
  "cpp": [
   "native/src/render/materials.cpp",
   "native/src/render/materials.hpp",
   "native/src/render/raster_ops.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_materials.cpp",
   "native/tests/gui/test_gui_materials.cpp",
   "native/tests/unit/test_material_stamp.cpp"
  ],
  "missing": [
   "import_image of Pillow-only picture formats (netpbm, ICO, CUR, ICNS, QOI, SGI, DDS, JPEG 2000, AVIF) are said not read by this build (NotYetPorted image_format): a recorded limit"
  ],
  "note": "Catalog, library, folders, packs and stamping ported; all_tags() (unused in Python) has no C++ counterpart."
 },
 "src/genko/materials/builtin.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/materials.cpp",
   "native/resources/materials/catalog.json"
  ],
  "tests": [
   "native/tests/gui/test_gui_materials.cpp",
   "native/tests/gui/test_gui_effects.cpp"
  ],
  "note": "Not ported as code: generated() is run once by tools/migration/material_prebuild.py into the packaged catalog.json (marks, props, scene line art, brush presets, 3D prims)."
 },
 "src/genko/mcp/__init__.py": {
  "verdict": "not_applicable",
  "note": "Package docstring only; the server is src/genko/mcp/server.py."
 },
 "src/genko/mcp/server.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/api/mcp_tools.cpp",
   "native/src/api/mcp_server.cpp",
   "native/src/api/mcp_stdio.cpp",
   "native/src/api/mcp_http.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_mcp.cpp",
   "native/tests/contract/test_contract_pipeline.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp"
  ],
  "note": "All 46 tools and 4 resources over stdio (plus Streamable HTTP, C++ only)."
 },
 "src/genko/merge.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/ops_bookpages.cpp",
   "native/src/storage/asset_store.cpp",
   "native/src/app/main_window_book.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_book.cpp",
   "native/tests/gui/test_gui_book.cpp"
  ],
  "note": "import_pages op and copy_assets."
 },
 "src/genko/mesh3d.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/mesh3d.cpp",
   "native/src/core/linalg3.cpp",
   "native/src/render/ops_3d.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/contract/test_contract_3d_render.cpp"
  ],
  "note": "Meshes, camera, shaded raster, seen lines, OBJ and glTF/GLB/VRM reading."
 },
 "src/genko/migrate.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/storage/reader.cpp",
   "native/src/storage/migrate.cpp"
  ],
  "tests": [
   "native/tests/e2e/test_migrate_e2e.cpp",
   "native/tests/contract/test_contract_storage.cpp"
  ],
  "note": "v1-v3 payloads read with migrate_payload's rules (reader.cpp); `genko migrate` converts to v4."
 },
 "src/genko/models.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/model.cpp",
   "native/src/core/model.hpp",
   "native/src/core/strokes.cpp",
   "native/src/core/ids.cpp"
  ],
  "tests": [
   "native/tests/unit/test_model.cpp",
   "native/tests/unit/test_strokes.cpp"
  ],
  "note": "Episode/Page/Layer/Frame/StoryLine/Stroke/PageSpec and stroke packing ported as core::Document types."
 },
 "src/genko/nombre.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/ops_book.cpp",
   "native/src/render/page.cpp",
   "native/src/app/book_dialogs.cpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_book.cpp",
   "native/tests/contract/test_contract_raster_render.cpp"
  ],
  "note": "Nombre settings op, numbering, placement and drawing."
 },
 "src/genko/ops.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/command_bus.cpp",
   "native/src/core/ops_util.cpp",
   "native/src/core/ops_pages.cpp",
   "native/src/core/ops_layers.cpp",
   "native/src/core/ops_frames.cpp",
   "native/src/core/ops_strokes.cpp",
   "native/src/core/ops_lines.cpp",
   "native/src/core/ops_schema.cpp",
   "native/src/render/ops_registry.cpp",
   "native/src/render/raster_ops.cpp"
  ],
  "tests": [
   "native/tests/unit/test_ops.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/test_contract_ops_cli.cpp"
  ],
  "note": "All 168 op names of the Python schemas are registered in C++ (undo and for_pages handled in command_bus), apply_ops compared against Python."
 },
 "src/genko/pack.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/formats/pack.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_export_cli.cpp"
  ],
  "note": "入稿セット export."
 },
 "src/genko/pagespec.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/ops_bookpages.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_book.cpp",
   "native/tests/gui/test_gui_book.cpp"
  ],
  "note": "spec_from and relayout behind set_page_spec."
 },
 "src/genko/persp3d.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/persp3d.cpp",
   "native/src/core/persp3d.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_3d_geometry.cpp"
  ],
  "note": "Perspective box geometry."
 },
 "src/genko/pipeline.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/ops_pages.cpp"
  ],
  "tests": [
   "native/tests/unit/test_ops.cpp",
   "native/tests/contract/test_contract_ops.cpp"
  ],
  "note": "advance()'s name gate (ink only after name_ok) lives in the advance op."
 },
 "src/genko/placement.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/placement.cpp",
   "native/src/render/panels.cpp",
   "native/src/render/placed.cpp"
  ],
  "tests": [
   "native/tests/unit/test_placed.cpp",
   "native/tests/contract/test_contract_studio.cpp"
  ],
  "note": "Bleed outlines, clip boxes and fit_rect for placed pictures."
 },
 "src/genko/plugins.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/plugins.cpp",
   "native/src/render/plugins.hpp",
   "native/src/app/plugins_dialog.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_plugins.cpp",
   "native/tests/unit/test_native_plugins.cpp"
  ],
  "note": "Python plugins kept but run out of process (opt-in, per-plugin consent, SPEC COMP-04); native plugins added."
 },
 "src/genko/poses.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/poses.cpp",
   "native/src/core/poses.hpp"
  ],
  "tests": [
   "native/tests/unit/test_ops_3d.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp"
  ],
  "note": "Pose library."
 },
 "src/genko/prim3d.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/prim3d.cpp",
   "native/src/core/prim3d.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/contract/test_contract_3d_render.cpp"
  ],
  "note": "Boxes, stairs, floors and scenes traced to lines."
 },
 "src/genko/profiles.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/formats/profiles.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_export_cli.cpp"
  ],
  "note": "Webtoon and SNS exports; probe() (only for Python tests) not ported."
 },
 "src/genko/psd.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/formats/psd_export.cpp",
   "native/src/render/psd.cpp",
   "native/src/render/page.cpp",
   "native/src/render/colour.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_psd.cpp",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/unit/test_psd_text.cpp"
  ],
  "note": "PSD/PSB writer, page_layers and the reader (Lab refused as in Python)."
 },
 "src/genko/raster.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/raster.cpp",
   "native/src/render/raster.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/unit/test_raster_ops.cpp"
  ],
  "note": "ensure/save_raster, bake_stroke, erase_raster."
 },
 "src/genko/render.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/page.cpp",
   "native/src/render/composite.cpp",
   "native/src/render/panels.cpp",
   "native/src/render/draw.cpp",
   "native/src/render/placed.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/unit/test_render_page.cpp",
   "native/tests/contract/test_contract_render_cli.cpp"
  ],
  "note": "render_page/frame/spread, layer_image, gradients, layer_effects, borders, to_bitonal ported; a placed picture stored as a planar TIFF is reported not_yet_ported."
 },
 "src/genko/rulers.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/rulers.cpp",
   "native/src/core/ops_rulers.cpp"
  ],
  "tests": [
   "native/tests/unit/test_rulers.cpp",
   "native/tests/contract/test_contract_rulers.cpp"
  ],
  "note": "Rulers, snapping and their ops."
 },
 "src/genko/scanner.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/app/scanner.cpp",
   "native/src/app/scanner.hpp"
  ],
  "tests": [
   "native/tests/gui/test_gui_book.cpp",
   "native/tests/gui/test_gui_effects.cpp"
  ],
  "note": "WIA / SANE scanimage, with Python's error words."
 },
 "src/genko/screentone.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/screentone.cpp",
   "native/src/render/tones.cpp"
  ],
  "tests": [
   "native/tests/unit/test_placed.cpp",
   "native/tests/contract/test_contract_render.cpp"
  ],
  "note": "Monochrome finish (levels, line mask, steps, AM/FM dots); unused effective_lpi/tone_area not ported."
 },
 "src/genko/selection.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/selection.cpp",
   "native/src/render/selection.hpp"
  ],
  "tests": [
   "native/tests/unit/test_raster_ops.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/gui/test_gui_select.cpp"
  ],
  "note": "Areas, lift/drop/transform of lines, patches and pixels; the alias stroke_points_of() is not separately present."
 },
 "src/genko/selops.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/selection.cpp",
   "native/src/core/areas.cpp"
  ],
  "tests": [
   "native/tests/unit/test_raster_ops.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/gui/test_gui_select.cpp"
  ],
  "note": "Area language (poly/mask/ellipse/combine/resolve/outline/stroke_area)."
 },
 "src/genko/server.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/api/headless_server.cpp",
   "native/src/api/http_server.cpp",
   "native/src/api/server_cli.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_mcp.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp",
   "native/tests/unit/test_http_server.cpp"
  ],
  "note": "Every route of `genko serve`, tokens, hosts, origins and OPTIONS."
 },
 "src/genko/stroke.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/stroke_geom.cpp",
   "native/src/core/stroke_tools.cpp",
   "native/src/render/stroke.cpp"
  ],
  "tests": [
   "native/tests/unit/test_stroke_geom.cpp",
   "native/tests/contract/test_contract_draw.cpp"
  ],
  "note": "stabilize/taper/fit_curve/pressure curve/pack_point/eraser splitting in core, stamp_polyline/draw_stroke_mm in render."
 },
 "src/genko/studio/__init__.py": {
  "verdict": "not_applicable",
  "note": "Package docstring only."
 },
 "src/genko/studio/adopt.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/evaluate.cpp",
   "native/src/api/studio_cli.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/unit/test_service.cpp"
  ],
  "note": "adopt-drafts (M0 sidecar files into the book)."
 },
 "src/genko/studio/atari.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/atari.cpp",
   "native/src/service/studio_atari.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/unit/test_service.cpp"
  ],
  "note": "Hand-drawn name analysis/import (analyze_name, import_name)."
 },
 "src/genko/studio/blocking.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/blocking.cpp",
   "native/src/service/blocking.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_pipeline.cpp"
  ],
  "note": "Figure boxes from the panel brief; tested only through letter/finish/review cases."
 },
 "src/genko/studio/claims.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/studio/files.cpp",
   "native/src/studio/files.hpp"
  ],
  "tests": [
   "native/tests/unit/test_studio_files.cpp"
  ],
  "note": "O_EXCL leases on work items."
 },
 "src/genko/studio/cli.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/api/studio_cli.cpp",
   "native/src/api/cli.cpp",
   "native/src/api/mcp_stdio.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_mcp.cpp"
  ],
  "note": "All 20 `genko studio` subcommands and `genko mcp`."
 },
 "src/genko/studio/drafts.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/evaluate.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/unit/test_service.cpp"
  ],
  "note": "Read side of Drafts (the only use since M3) inside adopt_drafts; the unused writers are not ported."
 },
 "src/genko/studio/evaluate.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/evaluate.cpp",
   "native/src/service/evaluate_tests.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/unit/test_service.cpp"
  ],
  "note": "Evaluation sets (blind identification, panel detection scores)."
 },
 "src/genko/studio/finish.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/finish.cpp",
   "native/src/service/finish.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_pipeline.cpp"
  ],
  "note": "finish_page (marks, effects, rain)."
 },
 "src/genko/studio/fxwords.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/fxwords.cpp",
   "native/src/service/fxwords.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp"
  ],
  "note": "Effect words; tested through lint and generation_request cases."
 },
 "src/genko/studio/genreq.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/genreq.cpp",
   "native/src/service/genreq.hpp",
   "native/src/studio/files.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/unit/test_service.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp"
  ],
  "note": "Generation requests, prompts and their files."
 },
 "src/genko/studio/importer.py": {
  "verdict": "partial",
  "cpp": [
   "native/src/service/importer.cpp",
   "native/src/service/importer.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/unit/test_service.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp"
  ],
  "missing": [
   "importing pictures in Pillow-only picture formats (netpbm, ICO, CUR, ICNS, QOI, SGI, DDS, JPEG 2000, AVIF) are said not read by this build (NotYetPorted image_format): a recorded limit"
  ],
  "note": "PNG/JPEG/GIF/BMP/WebP/TIFF/PSD candidates imported with Python's checks."
 },
 "src/genko/studio/issues.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/studio/schemas.hpp",
   "native/src/service/result.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp"
  ],
  "note": "Issue, error/warning, has_errors."
 },
 "src/genko/studio/jobs.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/jobs.cpp",
   "native/src/service/jobs.hpp"
  ],
  "tests": [
   "native/tests/unit/test_service.cpp",
   "native/tests/contract/test_contract_mcp.cpp"
  ],
  "note": "Background jobs."
 },
 "src/genko/studio/jsonschema_lite.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/studio/schemas.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp"
  ],
  "note": "JSON Schema subset checker."
 },
 "src/genko/studio/jsonutil.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/studio/files.cpp"
  ],
  "tests": [
   "native/tests/unit/test_studio_files.cpp"
  ],
  "note": "canonical_json, content_hash, atomic write/read JSON."
 },
 "src/genko/studio/layout.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/layout.cpp",
   "native/src/app/templates.cpp",
   "native/src/core/ops_studio.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/gui/test_gui_actions.cpp"
  ],
  "note": "Layout compile, built-in and personal templates (app limits in applying them listed under app/dialogs.py)."
 },
 "src/genko/studio/letter.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/letter.cpp",
   "native/src/service/letter.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_pipeline.cpp"
  ],
  "note": "Automatic lettering (propose_lines)."
 },
 "src/genko/studio/lint.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/lint.cpp",
   "native/src/service/lint.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp"
  ],
  "note": "Bible/script/name-plan lint; tested through set_bible/set_script/submit_name cases."
 },
 "src/genko/studio/preflight.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/preflight.cpp",
   "native/src/formats/checks.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_checks.cpp",
   "native/tests/contract/test_contract_service.cpp"
  ],
  "note": "Studio preflight."
 },
 "src/genko/studio/presence.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/studio/files.cpp"
  ],
  "tests": [
   "native/tests/unit/test_studio_files.cpp",
   "native/tests/contract/test_contract_studio_gui.cpp"
  ],
  "note": "presence.json (actor_for, others, check, touch)."
 },
 "src/genko/studio/review.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/review.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/unit/test_service.cpp"
  ],
  "note": "review_page / record_review."
 },
 "src/genko/studio/schemas.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/studio/schemas.cpp",
   "native/src/studio/schemas.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp"
  ],
  "note": "Input schemas and checks."
 },
 "src/genko/studio/service.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/studio_service.cpp",
   "native/src/service/tool_table.cpp",
   "native/src/service/service_write.cpp",
   "native/src/service/studio_art.cpp",
   "native/src/service/studio_output.cpp",
   "native/src/service/human_service.cpp",
   "native/src/service/briefs.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/unit/test_service.cpp",
   "native/tests/contract/test_contract_pipeline.cpp"
  ],
  "note": "StudioService (every agent tool in the table) and HumanService; unused result_dict() not ported."
 },
 "src/genko/studio/state.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/studio/state.cpp",
   "native/src/studio/state.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_studio.cpp"
  ],
  "note": "Studio state helpers."
 },
 "src/genko/studio/studio_ops.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/ops_studio.cpp",
   "native/src/core/studio.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/unit/test_studio_ops.cpp"
  ],
  "note": "All studio ops incl. region ops, compared against Python."
 },
 "src/genko/studio/toollog.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/studio/files.cpp"
  ],
  "tests": [
   "native/tests/unit/test_studio_files.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp"
  ],
  "note": "studio/logs/tools.jsonl."
 },
 "src/genko/studio/tools_registry.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/studio/files.cpp",
   "native/src/api/studio_cli.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp"
  ],
  "note": "<config>/tools.json via `genko studio tools`."
 },
 "src/genko/studio/worklist.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/worklist.cpp",
   "native/src/service/worklist.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp"
  ],
  "note": "next() work list."
 },
 "src/genko/studio/xycut.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/atari.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp"
  ],
  "note": "Otsu/XY-cut panel detection inside atari; tested through analyze_name/import_name."
 },
 "src/genko/stylecat.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/core/stylecat.cpp",
   "native/src/service/stylecat.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/gui/test_gui_studio.cpp"
  ],
  "note": "Style catalog and use_style."
 },
 "src/genko/tategaki.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/text/tategaki.cpp",
   "native/src/render/text/tategaki.hpp",
   "native/src/core/ruby.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/unit/test_ruby_render.cpp"
  ],
  "note": "Vertical layout, kinsoku, ruby and 傍点 byte for byte; unused paste_vertical not ported."
 },
 "src/genko/threeops.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/ops_3d.cpp",
   "native/src/render/ops_3d.hpp"
  ],
  "tests": [
   "native/tests/unit/test_ops_3d.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_3d_random.cpp"
  ],
  "note": "The 3D ops (figures, heads, hands, boxes, models, camera, trace)."
 },
 "src/genko/timelapse.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/timelapse.cpp",
   "native/src/render/movie.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_export_cli.cpp",
   "native/tests/gui/test_gui_anim.cpp"
  ],
  "note": "Recording on save and export as WebP/GIF/APNG/MP4."
 },
 "src/genko/tokens.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/api/tokens.cpp",
   "native/src/core/config_dir.cpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_mcp.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp"
  ],
  "note": "HTTP tokens (add/list/revoke, check) and the config folder."
 },
 "src/genko/tones.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/tones.cpp",
   "native/src/render/ops_tones.cpp",
   "native/src/render/npcompat.cpp"
  ],
  "tests": [
   "native/tests/unit/test_tones_effects.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/test_contract_tone_render.cpp"
  ],
  "note": "Screen tones, their ops and drawing."
 },
 "src/genko/upscale.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/service/upscale.cpp",
   "native/src/service/upscale.hpp"
  ],
  "tests": [
   "native/tests/unit/test_service.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp"
  ],
  "note": "Registered upscalers and Genko's own enlargement."
 },
 "src/genko/vectorize.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/vectorize.cpp",
   "native/src/render/vectorize.hpp"
  ],
  "tests": [
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/unit/test_raster_ops.cpp"
  ],
  "note": "trace_layer behind convert_layer paint->pen (tested by its raster cases)."
 },
 "src/genko/warp.py": {
  "verdict": "ported",
  "cpp": [
   "native/src/render/warp.cpp",
   "native/src/render/warp.hpp"
  ],
  "tests": [
   "native/tests/unit/test_raster_ops.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp"
  ],
  "note": "Perspective and mesh warps of lines and pixels."
 }
}
TEST_CPP: dict[str, dict] = {
 "tests/test_agent_economy.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_mcp.cpp",
   "tools/migration/make_mcp_cases.py",
   "tools/migration/mcp_harness.py",
   "native/tests/contract/test_contract_service.cpp",
   "tools/migration/make_service_cases.py",
   "native/tests/unit/test_command_bus.cpp",
   "native/tests/contract/test_contract_save.cpp"
  ],
  "note": "tools/list schemas, inspect ops/drawing/snapshot, failures and warnings, next items, export_status waits and sheet requests together are compared with Python (MCP contract skipped without GENKO_MCP_PYTHON). Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_ai_feedback.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_pipeline.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp",
   "native/tests/unit/test_ledger_service_a.cpp",
   "tools/migration/make_service_cases.py"
  ],
  "note": "Props written by name (prop_by_name) are in the service flow job. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_ledger_service_a.cpp"
 },
 "tests/test_b_lettering.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/lines_cases.json",
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_pipeline.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/unit/test_ledger_service_a.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/pipeline_harness.py",
   "tools/migration/render_harness.py"
  ],
  "note": "phrase_columns/phrases/without_periods/text_layout and balloon styles are compared with Python; measure is checked against Python's numbers; tail_to/obvious/Room are only exercised indirectly through name-compile and finish parity. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_ledger_service_a.cpp"
 },
 "tests/test_battle_report.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/unit/test_ledger_service_a.cpp",
   "native/tests/unit/test_render_page.cpp",
   "native/tests/unit/test_service.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/make_studio_cases.py"
  ],
  "note": "Lost jobs (unit), moved regions with set_placement, check pages, crop/dpi_short/bad_crop and the nombre's white edge (nombre fixtures from Python) are covered. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_ledger_service_a.cpp"
 },
 "tests/test_c_extras.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/unit/test_ledger_service_a.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "The name_plan@1 schema itself is compared with Python's (studioFilesLikePython). Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite. M7: the checks no other C++ test made are in test_ledger_service_a.cpp"
 },
 "tests/test_chat_approval.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp",
   "native/tests/unit/test_ledger_service_a.cpp",
   "native/tests/unit/test_service.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/make_studio_cases.py"
  ],
  "note": "The flow job of the service contract compares message_required, gate_not_available, chat_approval_off, a person's off/on switch and recorded chat approvals byte for byte with Python; the agent's allow_chat_approval refusal is in studio_cases and the MCP e2e. M7: the checks no other C++ test made are in test_ledger_service_a.cpp"
 },
 "tests/test_cli.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_save.cpp",
   "native/tests/contract/test_contract_export_cli.cpp",
   "native/tests/contract/test_contract_cli.cpp",
   "native/tests/contract/test_contract_ops_cli.cpp",
   "native/tests/e2e/test_cli_e2e.cpp",
   "native/tests/e2e/test_ops_e2e.cpp",
   "native/tests/contract/test_contract_lines.cpp"
  ],
  "note": "new (newMatchesPython), export png/tiff/pdf, apply+inspect and schema compared with python -m genko; add_line itself is compared through the bus (test_contract_lines), not through `genko apply`. Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite."
 },
 "tests/test_effect_clearing.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_pipeline.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/test_contract_tone_render.cpp",
   "native/tests/contract/tone_cases.hpp",
   "native/tests/gui/test_gui_effects.cpp",
   "native/tests/unit/test_ledger_service_a.cpp",
   "tools/migration/tone_harness.py"
  ],
  "note": "avoid/within geometry (lines and beta_flash fill) by pixel parity on random books; avoid refusal in fixed cases; the window's inside/clear/forget actions in test_gui_effects. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_ledger_service_a.cpp"
 },
 "tests/test_f1.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/book_cases.json",
   "native/tests/contract/test_contract_book.cpp",
   "native/tests/contract/test_contract_checks.cpp",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_export_cli.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_raster_render.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_storage.cpp",
   "native/tests/gui/test_gui_book.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_look.cpp",
   "native/tests/unit/test_model.cpp"
  ],
  "note": "Presets/margins (pyref unit tables), legacy books, exports by area, crop marks/bleed/spreads (render parity), set_page_spec moves, paint layers on new paper, checks, and changing the paper from the window are covered. Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_f2.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_anim.cpp",
   "native/tests/gui/test_gui_effects.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_output.cpp",
   "native/tests/gui/test_gui_paper.cpp",
   "native/tests/unit/test_ops.cpp"
  ],
  "note": "A person inks before name_ok (ops contract, test_ops strictGates). Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_f3.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_brushes.cpp",
   "native/tests/gui/test_gui_layout.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_studio.cpp",
   "native/tests/gui/test_gui_workspace.cpp"
  ],
  "note": "Tool settings follow the tool (eachToolsPage, eraser size) and agent panels only for agent books (plainBookHasNoStudio, requestsArrive) are covered. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_f4.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_canvas.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_pen.cpp",
   "native/tests/perf/test_perf_app.cpp"
  ],
  "note": "The live line equals the committed line pixel for pixel for every brush; the pen's eraser end erases and gives the pen back. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp; the per-move time bound of the live line is AC-PERF's PERF-B (p95 33 ms, judged at the milestone exit)"
 },
 "tests/test_f5.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/lines_cases.json",
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "parse_marks/with_marks, emphasis dots and ruby, turned/slanted/bowed lines, hand-drawn balloons and Latin on its side are compared with Python (text/balloons/lines contracts). Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_f6.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_render.cpp",
   "tools/migration/render_harness.py",
   "native/tests/unit/test_paper.cpp",
   "native/tests/unit/test_material_stamp.cpp",
   "native/tests/contract/test_contract_brushes.cpp",
   "native/tests/gui/test_gui_layers.cpp",
   "native/tests/gui/test_gui_carried.cpp",
   "native/tests/gui/test_gui_select.cpp",
   "native/tests/gui/test_gui_brushes.cpp",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/gui/test_gui_output.cpp",
   "native/tests/gui/test_gui_look.cpp"
  ],
  "note": "Layer ops, masks, warp perspective/mesh, export pages/dialog/preflight, history, help, preferences and the side button all have C++ equivalents; define_brush 'another computer' is checked with paper brushes and the book's own brushes. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_frames.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/unit/test_model.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json"
  ],
  "note": "test_model newEpisodeAsPython (root panel = inner rect), splitMergeResizeKeepPythonNumbers (gutter geometry), readingOrderVerticalAndHorizontal (right-to-left)."
 },
 "tests/test_g1.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_checks.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/e2e/test_app_e2e.cpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_output.cpp",
   "native/tests/unit/test_render_page.cpp",
   "native/tests/unit/test_session.cpp"
  ],
  "note": "art_not_printed (checks contract) and undo one change at a time (test_session, test_gui_actions undoAndRedo) are covered. Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp; the 0.5 s bound for a line on a finished page is AC-PERF's PERF-C (p95 100 ms, judged at the milestone exit)"
 },
 "tests/test_g2.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_effects.cpp",
   "native/tests/gui/test_gui_layers.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_pen.cpp"
  ],
  "note": "Style box beside the tool and material tabs 素材/トーン/効果線 are covered. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_g3.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_carried.cpp",
   "native/tests/gui/test_gui_effects.cpp",
   "native/tests/gui/test_gui_layers.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_workspace.cpp"
  ],
  "note": "Layer menu (add, duplicate, merge, mask from selection) and line menu (shape, wrap, balloon pen, delete) are covered. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_g4.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/lines_cases.json",
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "Ruby across, styled runs, bold, italic, outline colour, wobble/double/spikes are compared pixel for pixel by the text and balloon contracts (render_harness text-cases/balloon-cases); refusals and save/reload by lines_cases.json; contract tests skip without the Python reference. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_g5.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_filters.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/test_contract_raster_render.cpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_paint.cpp",
   "tools/migration/geom3d_harness.py",
   "tools/migration/render_harness.py"
  ],
  "note": "gradient_fill (linear, radial, too-short refusal), levels/curve/hue tables, cylinder/stairs/floor + trace_prims + torus refusal and the gradient tool's ops are covered. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_g6.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_service.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/service_harness.py",
   "native/tests/contract/test_contract_mcp.cpp",
   "tools/migration/make_mcp_cases.py",
   "tools/migration/mcp_harness.py",
   "native/tests/e2e/test_mcp_api_e2e.cpp",
   "native/tests/unit/test_service.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_export.cpp"
  ],
  "note": "AGENT_OPS is compared through service `inspect ops` (and human-only ops refused in test_mcp_api_e2e anAgentCannotGetThrough); the drawing ops, strict gates and name_ok-by-AI by the op contracts; check/preflight/undo/export by service cases; MCP tools and export by mcp cases; guide words via `inspect drawing`/rules compared with Python (docs/AGENT.md is a Python-repo doc)."
 },
 "tests/test_h1.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/e2e/test_ops_e2e.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/gui/test_gui_paper.cpp",
   "native/tests/perf/test_perf_app.cpp",
   "native/tests/perf/test_perf_render.cpp",
   "native/tests/perf/test_soak_app.cpp",
   "native/tests/unit/test_journal.cpp",
   "native/tests/unit/test_model.cpp",
   "native/tests/unit/test_ops.cpp",
   "native/tests/unit/test_render_page.cpp",
   "native/tests/unit/test_storage.cpp",
   "native/tests/unit/test_strokes.cpp",
   "native/tests/unit/test_transaction.cpp"
  ],
  "note": "Copy-on-write (test_ops everyOp, test_model copyOnWritePages), packed/old-form strokes (test_strokes), unchanged blobs kept (test_storage), big op records as assets (test_transaction), ink_stroke_count (snapshot parity), zoom tiles at the zoom's dpi (test_gui_paper zoomTiles) are covered. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp; test_a_thick_book_stays_quick's bounds are AC-PERF's (open PERF-A 3 s, a line and Undo/Redo PERF-C 100 ms, a page turn PERF-D 500 ms; judged at the milestone exit); a small picture's 0.5 s is judged in test_perf_render; Python's private _touched_pages and parse cache are C++'s copy-on-write pages and storage::journal::read_lines_without_ops (test_journal)"
 },
 "tests/test_h2.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/lines_cases.json",
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/gui/test_gui_carried.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_output.cpp",
   "native/tests/gui/test_gui_select.cpp",
   "native/tests/gui/test_gui_workspace.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "Lettering form hidden until a line is chosen, the weight field, electric geometry/drawing and printing (PDF sheets, page field, 9 refused, Ctrl+P) are covered. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_h4.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_3d_render.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/gui/test_gui_carried.cpp",
   "native/tests/gui/test_gui_layers.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "tools/migration/geom3d_harness.py"
  ],
  "note": "Scenes (add_scene/edit_prim/trace_prims/delete_prim, castle refused, corridor projection) via 3D contracts; reference fills via raster_cases; auto actions via test_gui_carried. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_h5.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_mcp.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "tools/migration/make_mcp_cases.py",
   "tools/migration/make_service_cases.py"
  ],
  "note": "Snapshot fields are compared by the op contracts and inspect snapshot; MCP tool schemas (layer_id etc.) via test_contract_mcp list_tools; add_scene in AGENT_OPS via inspect ops; guide/doc text checks of SKILL.md/AGENT.md/integrations copy are Python-repo docs (guide text compared via inspect drawing). M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_hd_report.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_book.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_tone_render.cpp",
   "native/tests/gui/test_gui_effects.cpp",
   "native/tests/gui/test_gui_layout.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_look.cpp",
   "native/tests/gui/test_gui_paint.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "Bleed with round/dashed panels is in the random render books, join_lines and trace_prims in the 3D contracts, pages before covers in book_cases, grey gradients on mono pages in render books, screen dots in tone_render, styling chosen characters and the wheel in GUI tests. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_headless.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_save.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_cli.cpp",
   "native/tests/unit/test_command_bus.cpp"
  ],
  "note": "Snapshot/reply parity with Python's apply_ops (split_frame, name_ok, advance unapproved to ink refused, add_stroke name/ink, unknown op). Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite."
 },
 "tests/test_i0.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/lines_cases.json",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_3d_render.cpp",
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/gui/test_gui_carried.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_output.cpp"
  ],
  "note": "Default vertical wrap and diagonal refusal, electric balloons' inner room (drawn byte for byte; box_size), guides bound to panels, printing sheets/actual size/spreads/preview and the action manager are covered. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_io.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/unit/test_storage.cpp",
   "native/tests/contract/test_contract_storage.cpp"
  ],
  "note": "Every successful ops/lines case is saved and read back and compared; test_storage v4RoundTrip/newBooksKeepTheirNumbers (split panels, story line). Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite."
 },
 "tests/test_j1.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/gui/test_gui_canvas.cpp",
   "native/tests/gui/test_gui_ledger_a.cpp",
   "native/tests/gui/test_gui_look.cpp",
   "native/tests/gui/test_gui_workspace.cpp"
  ],
  "note": "Command search, command bar, quick access, workspaces, dark theme, held Alt, cursor kind and preferences are covered by same-named C++ GUI tests. M7: the checks no other C++ test made are in test_gui_ledger_a.cpp"
 },
 "tests/test_j10.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/psd_cases.json",
   "native/tests/contract/test_contract_anim.cpp",
   "native/tests/contract/test_contract_colour.cpp",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_export_cli.cpp",
   "native/tests/contract/test_contract_psd.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_anim.cpp",
   "native/tests/gui/test_gui_book.cpp",
   "native/tests/gui/test_gui_color.cpp",
   "native/tests/gui/test_gui_effects.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/gui/test_gui_output.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/pyref_harness.py",
   "tools/migration/render_harness.py"
  ],
  "note": "PSD reading/import, PSD export, CMYK/profiles, layers, Kindle, timelapse, AGENT_OPS (service inspect ops) and the app's dialogs are covered. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_j11.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp"
  ],
  "note": "Only test_gui_actions theBooksAndWindows touches tabs and a second window. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_j12.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/anim_cases.json",
   "native/tests/contract/test_contract_anim.cpp",
   "native/tests/contract/test_contract_export_cli.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/gui/test_gui_anim.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "tools/migration/pyref_harness.py",
   "tools/migration/render_harness.py"
  ],
  "note": "Exposure ops/refusals, frames drawn and written out (GIF/APNG/WebP/PNGs/MP4 frame by frame vs Python), length/saving, agents (inspect ops), CLI export and the timeline GUI are covered. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_j2.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_paint.cpp",
   "native/tests/contract/paint_cases.json",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_rulers.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/tone_cases.hpp",
   "tools/migration/tone_harness.py",
   "native/tests/unit/test_rulers.cpp",
   "native/tests/gui/test_gui_paint.cpp",
   "native/tests/gui/test_gui_select.cpp",
   "native/tests/gui/test_gui_workspace.cpp",
   "native/tests/gui/test_gui_output.cpp",
   "native/tests/gui/test_gui_effects.cpp"
  ],
  "note": "add_shape and area language (union/subtract/intersect/invert/grow/feather/layer/colour/saved, store/forget_area) via contracts; guide snapping via rulers contract; GUI figure tool, Shift/Alt joins, selection pen/eraser/colour, stock, launcher, scales and sub-view via GUI tests."
 },
 "tests/test_j3.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/paint_cases.json",
   "native/tests/contract/test_contract_abr.cpp",
   "native/tests/contract/test_contract_brushes.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_paint.cpp",
   "native/tests/contract/test_contract_storage.cpp",
   "native/tests/gui/test_gui_brushes.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/gui/test_gui_paint.cpp",
   "native/tests/unit/test_material_stamp.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "Every brush, flat/picture tips, speed, AA, watercolour, bad definitions and to_dict (what define_brush stores) via the brushes contract; .abr via abr contract; post_smooth/erase whole via ops_cases; smudge via paint_cases; brush files and blend tool via GUI. define_brush itself is not run against Python. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_j4.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_vector.cpp",
   "native/tests/contract/vector_cases.json",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json",
   "native/tests/gui/test_gui_colours.cpp",
   "native/tests/gui/test_gui_paint.cpp"
  ],
  "note": "vector_edit actions and refusals, fill_gaps, colorsys sums, the colour panel, vector tool, transparent pen and layer eyedropper are covered."
 },
 "tests/test_j5.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/paint_cases.json",
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_filters.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_paint.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/test_contract_raster_render.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/gui/test_gui_color.cpp",
   "native/tests/gui/test_gui_layers.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/gui/test_gui_paint.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "Fill/gradient/correction layers, layer colour printing, border/water edges, blend modes, merge/group/move/set_layers, convert_layer, paper colour, liquify, nearest resampling and every filter are covered by op and render contracts. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_j6.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/lines_cases.json",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/ruler_cases.json",
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_checks.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_rulers.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/test_contract_tone_render.cpp",
   "native/tests/contract/tone_cases.hpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_guides.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_materials.cpp",
   "native/tests/unit/test_rulers.cpp",
   "tools/migration/render_harness.py",
   "tools/migration/tone_harness.py"
  ],
  "note": "Bowed panels, border styles, scale_x/gradient/path/variation selectors/yakumono/features letters, picture balloons, spikes/bumps/tails, effect paths, pattern tones, screens and moiré, curve rulers, layer rulers and eye level are covered by contracts. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_j7.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/lines_cases.json",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_materials.cpp",
   "native/tests/gui/test_gui_effects.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/gui/test_gui_materials.cpp",
   "native/tests/unit/test_material_stamp.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "Material counts/previews, marks on a layer, brush materials and packs (folder, zip, export, refusals) are covered. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_j8.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_3d_cli.cpp",
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_3d_render.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/gui/test_gui_guides.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "tools/migration/geom3d_harness.py"
  ],
  "note": "Body shape, joints/presets/hands, drags and IK, heads/hands/OBJ/glTF/VRM models and refusals, hidden lines, camera and light, render_prims tone are compared bit for bit with Python; GUI placing/posing in test_gui_guides. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_j9.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_book.cpp",
   "native/tests/contract/book_cases.json",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/lines_cases.json",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "tools/migration/pyref_harness.py",
   "native/tests/contract/test_contract_service.cpp",
   "tools/migration/make_service_cases.py",
   "native/tests/gui/test_gui_carried.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_book.cpp"
  ],
  "note": "for_pages under strict_gates is reached only through the random op sequences of test_contract_ops. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_k1.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_brushes.cpp",
   "native/tests/contract/test_contract_drawn_by_ops.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_plugins.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/tone_cases.hpp",
   "native/tests/gui/test_gui_anim.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/gui/test_gui_pen.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "Barrel rotation (add_stroke rotation, flat tips drawn), tip_rotation in brush to_dict, effect_to_layer + delete_stroke, and the sepia plugin with its refusals (test_contract_plugins) are covered. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_l1_style.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/contract/test_contract_studio_gui.cpp",
   "native/tests/gui/test_gui_studio.cpp",
   "native/tests/unit/test_ledger_service_a.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/make_studio_cases.py"
  ],
  "note": "The style job of the service contract (catalog browsing, use_style look/commit, status, newer version, back), studio_cases (set_style_catalog person-only once fixed, the pilot's tool not Genko's enlargement) and test_gui_studio (picker levels, search, marks, links handed over by a second start) cover the rest. M7: the checks no other C++ test made are in test_ledger_service_a.cpp"
 },
 "tests/test_lock.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/unit/test_lock.cpp",
   "native/tests/contract/test_contract_save.cpp"
  ],
  "note": "test_lock twoLocksInOneProcess / anotherProcess; Python and C++ locks exclude each other in test_contract_save."
 },
 "tests/test_m10.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_brushes.cpp",
   "native/tests/gui/test_gui_carried.cpp",
   "native/tests/gui/test_gui_layers.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_pen.cpp",
   "native/tests/unit/test_ops.cpp"
  ],
  "note": "Engine checks (widths, colour/opacity, order/hidden, borders, erase, layers, locked saved, strict gates, filter on a pen layer, edit_line, balloons) are contract-covered; GUI: place_new and the lines panel (test_gui_lines), image into the chosen panel (test_gui_carried), act_guides. Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp; the drag's 0.5 s bound is AC-PERF's PERF-B/PERF-C (judged at the milestone exit)"
 },
 "tests/test_m11.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/lines_cases.json",
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/package/test_package.cpp",
   "native/tests/unit/test_ledger_service_a.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "Faces, tate-chu-yoko, kinsoku, tracking/leading/ruby, every balloon shape, borders, fill none, joined balloons and tails are compared with Python byte for byte in the text and balloon contracts; style validation, saving and reorder_lines in lines_cases; the text tool, typing over, handles and the lines panel in test_gui_lines. M7: the checks no other C++ test made are in test_ledger_service_a.cpp"
 },
 "tests/test_m12.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_canvas.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "split_frame (tilted), cut_frame, move_gutter (width, stack index, too far, strict agent refusal) and set_frame (poly, border, back to the cut) are op contract cases; clipping to slanted panels and panel_clip are in the random render books; templates, border and bleed from the menu in test_gui_actions. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_m13.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_brushes.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_drawn_by_ops.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_raster_render.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/gui/test_gui_brushes.cpp",
   "native/tests/gui/test_gui_paint.cpp",
   "native/tests/gui/test_gui_select.cpp"
  ],
  "note": "Every brush, pressure gamma, fills (gap closing, reference), fill_area, transform_area, paste, delete_area, set_stroke_width, reshape_stroke and erase to_crossing are op contract cases (strict refusal included); the brush panel, eyedropper, fill/lasso fill, marquee, wand, reshape and the crossing eraser are in the GUI tests."
 },
 "tests/test_m14.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ruler_cases.json",
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_3d_render.cpp",
   "native/tests/contract/test_contract_rulers.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/gui/test_gui_guides.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/unit/test_rulers.cpp",
   "tools/migration/geom3d_harness.py",
   "tools/migration/tone_harness.py"
  ],
  "note": "test_rulers.cpp ports test_m14's ruler cases and test_contract_rulers compares snapping bit for bit; 3D ops, drags/IK, edges and proof-only drawing are 3D contract cases; placing, editing and listing rulers and boxes in the window are in test_gui_guides. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_m15.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_materials.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/test_contract_tone_render.cpp",
   "native/tests/contract/tone_cases.hpp",
   "native/tests/gui/test_gui_effects.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/gui/test_gui_materials.cpp",
   "native/tests/unit/test_material_stamp.cpp",
   "native/tests/unit/test_tones_effects.cpp",
   "tools/migration/tone_harness.py"
  ],
  "note": "test_tones_effects ports test_m15's black-share checks; tone and effect ops (gradients, at-region, effect_to_layer, visibility) and scraping (ops_cases erase scrape/soft) are compared with Python, the drawing pixel for pixel in tone_render; the library steps in test_contract_materials; the tone/effect/material panels in GUI tests. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_m16.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/unit/test_command_bus.cpp",
   "native/tests/unit/test_render_page.cpp",
   "native/tests/fixtures/render/nombre-cases.json",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_checks.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_book.cpp",
   "native/tests/gui/test_gui_output.cpp"
  ],
  "note": "Page ops are op contract cases; nombre placements come from Python's genko.nombre (fixture) and set_nombre's options and refusals are in test_command_bus; every check (outside trim/frame, overlap, small, art outside, empty page, low dpi, the folded preflight) is compared byte for byte in test_contract_checks; parse_script, the story editor, the page list and the checks panel are in GUI tests."
 },
 "tests/test_m1_report.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_anim.cpp",
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_book.cpp",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_export_cli.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_psd.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/gui/test_gui_output.cpp",
   "native/tests/unit/test_ledger_service_a.cpp",
   "native/tests/unit/test_service.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/render_harness.py"
  ],
  "note": "Masks in layer PNGs, colour layers grey on mono pages, filters on patches, check(), replace_text results, the animation in the snapshot, the audit, face boxes, sheet requests and thought trails are compared with Python. M7: the checks no other C++ test made are in test_ledger_service_a.cpp"
 },
 "tests/test_m1_security.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_cli.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_mcp.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_ops_cli.cpp",
   "native/tests/contract/test_contract_save.cpp",
   "native/tests/contract/test_contract_storage.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/e2e/test_cli_e2e.cpp",
   "native/tests/package/test_package.cpp",
   "native/tests/unit/test_http_server.cpp",
   "native/tests/unit/test_journal.cpp",
   "native/tests/unit/test_ledger_service_a.cpp",
   "native/tests/unit/test_lock.cpp",
   "native/tests/unit/test_session.cpp",
   "native/tests/unit/test_storage.cpp",
   "tools/migration/make_mcp_cases.py",
   "tools/migration/mcp_harness.py"
  ],
  "note": "The HTTP guard (token, Host, Origin, Content-Type, OPTIONS, paths and links outside the root, the actor from the token) is in the MCP contract's api job; locks across processes and older content in test_lock/test_contract_save; lock and line-lock refusals in ops/lines cases; unknown keys kept and newer versions refused in test_storage; --ascii and UTF-8 output in test_cli_e2e. M7: the checks no other C++ test made are in test_ledger_service_a.cpp; the apply cost with 50 changes to undo is test_session anEditCostsNoMoreWithADeepHistory"
 },
 "tests/test_m2_format.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/unit/test_storage.cpp",
   "native/tests/contract/test_contract_save.cpp",
   "native/tests/contract/test_contract_storage.cpp",
   "native/tests/e2e/test_migrate_e2e.cpp",
   "native/tests/e2e/test_save_e2e.cpp",
   "native/tests/e2e/test_ops_e2e.cpp",
   "native/tests/unit/test_transaction.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_ops_cli.cpp",
   "native/tests/contract/test_contract_book.cpp",
   "native/tests/contract/book_cases.json",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json",
   "native/tests/unit/test_raster_ops.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_text.cpp"
  ],
  "note": "By design the C++ build writes v4 (hashed assets and stroke blobs) and never writes v1-v3 books: the v2 in-place upgrade with project.v2.bak.json is replaced by `genko migrate` (originals untouched). Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_m2_group_b.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_mcp.cpp",
   "tools/migration/make_mcp_cases.py",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/unit/test_service.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "tools/migration/make_service_cases.py",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_psd.cpp",
   "native/tests/contract/test_contract_pipeline.cpp",
   "tools/migration/pipeline_harness.py"
  ],
  "note": "Conversations held back once (book_in_use), undo by session and a bad session are in the MCP client jobs; presence in studioFilesLikePython; jobs in test_service and the art job's export_status; PSD writing/reading byte for byte in the export and psd contracts; Genko's and a person's enlargers, the same enlargement once and the upscaled preflight in the art job; upscale_panel work items in the pipeline run."
 },
 "tests/test_m3_art.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_mcp.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_ops_cli.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/unit/test_ledger_service_b.cpp",
   "tools/migration/make_mcp_cases.py",
   "tools/migration/make_service_cases.py",
   "tools/migration/make_studio_cases.py",
   "tools/migration/studio_harness.py"
  ],
  "note": "Adoption, fit and placement, drafts before the name, briefs through split/merge with orphans, strict gates, pins/overrides/regions, sheet approval and locking, fix tickets and unadopt, place_asset and import_image refusals and adopt-drafts are compared with Python (studio_cases, service art/flow/drafts jobs); placed pictures (cover, contain, drafts, missing, colour) are drawn pixel for pixel. M7: the checks no other C++ test made are in test_ledger_service_b.cpp"
 },
 "tests/test_m3_report.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_anim.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_pipeline.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_storage.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/e2e/test_migrate_e2e.cpp",
   "native/tests/unit/test_ledger_service_b.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/make_studio_cases.py",
   "tools/migration/pipeline_harness.py"
  ],
  "note": "CRLF project.json then Undo (test_migrate_e2e), the finish's focus lines aimed at reported faces (pipeline), fix_page/fix_panel items, resolving and reopening tickets and the agent's refusals (flow job, studio_cases), effects with ids in the snapshot (tone ops), outside_panels warnings (ops_cases) and timelapse/GIF frame timing (test_contract_anim) are compared with Python. M7: the checks no other C++ test made are in test_ledger_service_b.cpp"
 },
 "tests/test_m4_pipeline.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_mcp.cpp",
   "native/tests/contract/test_contract_pipeline.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp",
   "native/tests/unit/test_ledger_service_b.cpp",
   "tools/migration/make_mcp_cases.py",
   "tools/migration/make_service_cases.py",
   "tools/migration/pipeline_harness.py"
  ],
  "note": "test_contract_pipeline runs test_m4_pipeline's 8-page making (FakeAgent over MCP, ScriptedHuman via genko studio) on the C++ build and on Python and compares every call, the book, audit, stats and the exported files; the agent's gate refusals and the tool list are in the MCP contract and test_mcp_api_e2e (skipped without GENKO_MCP_PYTHON). M7: the checks no other C++ test made are in test_ledger_service_b.cpp"
 },
 "tests/test_m4_units.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_mcp.cpp",
   "native/tests/contract/test_contract_pipeline.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp",
   "native/tests/perf/test_perf_pipeline.cpp",
   "native/tests/unit/test_ledger_service_b.cpp",
   "native/tests/unit/test_service.cpp",
   "tools/migration/make_mcp_cases.py",
   "tools/migration/make_service_cases.py",
   "tools/migration/service_harness.py",
   "tools/migration/studio_harness.py"
  ],
  "note": "Generation requests, imports, claims, preflight/proof, finish, HTTP assets/request files, CLI tools/call and PERF-F (p95 <= 2 s) are covered by service/MCP/pipeline parity. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_ledger_service_b.cpp"
 },
 "tests/test_m5.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_mcp.cpp",
   "native/tests/contract/test_contract_pipeline.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/unit/test_ledger_service_b.cpp",
   "tools/migration/make_mcp_cases.py",
   "tools/migration/make_service_cases.py",
   "tools/migration/make_studio_cases.py",
   "tools/migration/pipeline_harness.py"
  ],
  "note": "The pilot page and the style lock (pipeline run, studio cases), the tool log and stats (MCP client jobs, pipeline end), review.html and a question closed with a reply becoming a fix (art/flow jobs), the D5 blind sample and score (art and cli jobs) and the agent's art approval refused (studio cases) are compared with Python. M7: the checks no other C++ test made are in test_ledger_service_b.cpp"
 },
 "tests/test_m6.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_export_cli.cpp",
   "native/tests/contract/test_contract_pipeline.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/contract/test_contract_tone_render.cpp",
   "native/tests/unit/test_ledger_service_b.cpp",
   "native/tests/unit/test_tones_effects.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/render_harness.py",
   "tools/migration/studio_harness.py",
   "tools/migration/tone_harness.py"
  ],
  "note": "Screens and their black share (tone_render, test_tones_effects), the mono finish of placed art and colour pages left alone (placedPicturesLikePython), line art derived and adopted to ink (art job, studio cases), balloons/ruby/sfx/effects (balloon, text, tone contracts), balloons off reported faces (pipeline finish), safe names, EPUB, PSD and bitonal TIFF files (export contracts, ship job) are compared with Python. M7: the checks no other C++ test made are in test_ledger_service_b.cpp"
 },
 "tests/test_m7_gui.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_studio_gui.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_look.cpp",
   "native/tests/gui/test_gui_studio.cpp",
   "native/tests/unit/test_session.cpp"
  ],
  "note": "approveName/replyToQuestion/panelView/library (test_gui_studio), another writer's change kept (test_session), start screen (test_gui_look). M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_m7_session.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/contract/test_contract_studio_gui.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/gui/test_gui_studio.cpp",
   "native/tests/unit/test_session.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/make_studio_cases.py"
  ],
  "note": "Rebase/conflicts/undo through the journal (test_session), inbox/progress/approve_ops/send_back_ops vs Python (test_contract_studio_gui), person-only ops refused for agents (make_studio_cases 'reject_sheet by an agent'). Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_m8.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio_gui.cpp",
   "native/tests/gui/test_gui_studio.cpp",
   "native/tests/unit/test_ledger_service_b.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/service_harness.py"
  ],
  "note": "The atari job compares import_name (alignment, refusals, draft layer), analyze_name with settings, propose_lines and its refusals, proposals, accepting (with force), the atari overlay render and review.html; the cli job the same commands; the approval box's 確定/却下 of a proposal is in test_gui_studio. M7: the checks no other C++ test made are in test_ledger_service_b.cpp"
 },
 "tests/test_m9.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_export_cli.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/unit/test_ledger_service_b.cpp",
   "tools/migration/geom3d_harness.py",
   "tools/migration/make_service_cases.py",
   "tools/migration/render_harness.py"
  ],
  "note": "Mannequin skeletons, presets and pose_mannequin are compared bit for bit in the 3D contracts; finish off and colour pages unscreened in placedPicturesLikePython and the ship job's screen exports; webtoon, SNS (spreads, long edge), the CLI's webtoon and a person's screen exports (preflight guard, gif refused) in the export contracts and ship job. M7: the checks no other C++ test made are in test_ledger_service_b.cpp"
 },
 "tests/test_migrate.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_storage.cpp",
   "native/tests/contract/test_contract_cli.cpp",
   "native/tests/e2e/test_migrate_e2e.cpp"
  ],
  "note": "The v1 legacy book's full snapshot (four default layers, name not exportable, story) equals Python's. Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite."
 },
 "tests/test_ops.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_save.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_ops_cli.cpp",
   "native/tests/unit/test_command_bus.cpp",
   "native/tests/unit/test_ops.cpp",
   "native/tests/unit/test_transaction.cpp"
  ],
  "note": "Rollback ('failure after changes', failedBatchChangesNothing), dry runs, undo (ops_cli undoAsOp, test_transaction), edit/delete_line, delete/duplicate_page, delete_stroke all compared with Python. Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite."
 },
 "tests/test_p1.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/lines_cases.json",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_balloons.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_cli.cpp",
   "native/tests/e2e/test_cli_e2e.cpp"
  ],
  "note": "move_line, merge/resize/set_frame (bleed, clip, border), put_raster saved and read back, balloons drawn pixel-for-pixel, schema equals Python's (C++ embeds native/src/core/data/ops_schema.json rather than docs/ops.schema.json). Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite."
 },
 "tests/test_p2.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/test_contract_raster_render.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/tone_cases.hpp",
   "native/tests/contract/test_contract_tone_render.cpp"
  ],
  "note": "flood_fill, add_tone/delete_tone and focus/speed effects compared as ops and as pixels. Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite."
 },
 "tests/test_p3.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/unit/test_model.cpp",
   "native/tests/unit/test_render_page.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_export_cli.cpp",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_ops.cpp"
  ],
  "note": "Presets (test_model), nombre visible in print (nombreVisible), tiff/pdf/strip files byte-equal to Python's, set_autosave saved and read back. Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite."
 },
 "tests/test_p4.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_drawn_by_ops.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/tone_cases.hpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_3d_render.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/contract/studio_cases.json",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_mcp.cpp",
   "tools/migration/make_mcp_cases.py"
  ],
  "note": "Pressure width, simplify/edit_stroke, set_ruler + prim drawn in name only, lt_convert, tickets, PSD/EPUB exports and /openapi.json are covered by contracts."
 },
 "tests/test_p5_balloon.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_text.cpp",
   "tools/migration/render_harness.py",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/lines_cases.json"
  ],
  "note": "Vertical text and ruby placement compared pixel for pixel with Python. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_p5_erase.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_drawn_by_ops.cpp"
  ],
  "note": "erase_raster on a pen layer cutting the line ('erase_raster cut through', 'pen layer') and drawn erasing are compared with Python."
 },
 "tests/test_p5_inspect_stroke.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_cli.cpp",
   "native/tests/e2e/test_cli_e2e.cpp"
  ],
  "note": "The compact snapshot is compared in every op reply; `genko inspect --stroke` checked in test_cli_e2e inspectStroke."
 },
 "tests/test_p5_layers.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json"
  ],
  "note": "reorder_layers and set_layer visible/opacity cases."
 },
 "tests/test_p5_lt.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json"
  ],
  "note": "lt_convert with the default (adaptive) method and the other methods are compared with Python."
 },
 "tests/test_p5_mannequin.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_3d_render.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "tools/migration/geom3d_harness.py"
  ],
  "note": "Mannequins drawn in proof/name and never in print, every pixel against Python."
 },
 "tests/test_p5_materials.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/lines_cases.json",
   "native/tests/gui/test_gui_materials.cpp"
  ],
  "note": "stamp_material dot-60-30 (tone layer settings) in lines_cases and doubleClickToneSaveReopenOutput."
 },
 "tests/test_p5_pack.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_export_cli.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "export_pack (shueisha) files compared byte for byte with Python. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_p5_psd.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_export.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "export_psd / export_psd_pages files compared byte for byte with Python (layer count included). Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_p5_ruler.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_rulers.cpp",
   "native/tests/contract/ruler_cases.json",
   "tools/migration/tone_harness.py"
  ],
  "note": "Snapping to a perspective ruler (old set_ruler and add_stroke snap_ruler) against Python, plus 500 random snap cases."
 },
 "tests/test_p5_spread.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_render.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "render_spread compared pixel for pixel on random books of both bindings. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_p5_stroke.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/gui/test_gui_pen.cpp",
   "native/tests/contract/test_contract_brushes.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "Pressured add_stroke, pack_point defaults (test_gui_pen) and gpen pressure drawing are covered."
 },
 "tests/test_p6_remaining.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_render.cpp",
   "tools/migration/render_harness.py",
   "native/tests/contract/test_contract_brushes.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_tone_render.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_3d_render.cpp"
  ],
  "note": "Legacy oil (= marker) drawing, onion skins in name only, page locks, set_meta font_path, tone angles and pose_mannequin are compared with Python."
 },
 "tests/test_p7_factory.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_render.cpp",
   "tools/migration/render_harness.py",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_filters.cpp",
   "native/tests/contract/test_contract_export.cpp"
  ],
  "note": "Layers added/deleted (ink refused), folders and parents, stabilize and taper are op contract cases; blend modes, clipping and lock_alpha are drawn pixel for pixel in the random render books; filter_raster kinds in raster_cases and the filters contract; spreads and PSD layer names in the export contract byte for byte."
 },
 "tests/test_p8_shell.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_drawn_by_ops.cpp"
  ],
  "note": "set_brush colour/curve then a line, the gpen pressure curve and a stroke past the gutter of a spread ('add_stroke past the gutter')."
 },
 "tests/test_p9_factory.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_save.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/unit/test_stroke_geom.cpp",
   "native/tests/gui/test_gui_pen.cpp"
  ],
  "note": "set_lt and lt_convert (raster contract), step_onion (test_contract_save's op batches), the mannequin's joints (3D contracts), print and PSD exports (export contract) and pack_point's tilt (test_stroke_geom against Python's numbers, test_gui_pen) are covered."
 },
 "tests/test_project.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/unit/test_model.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_save.cpp"
  ],
  "note": "new_episode/A4 inner rect/recto (test_model), reorder cases (ops contract), genko new vs Python."
 },
 "tests/test_quality_report.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_checks.cpp",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/test_contract_pipeline.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/unit/test_ledger_service_c.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/pipeline_harness.py",
   "tools/migration/render_harness.py",
   "tools/migration/studio_harness.py"
  ],
  "note": "PDF/EPUB files (export contract), the mono preflight, thought tails, the sheet request's written look, reviews needing checks (service jobs), faces lighter and clothes in beta, flat greys (placedPicturesLikePython), line art adopted on its picture (studio cases), grown balloons and hidden tails, faces cut by panels (lines/checks contracts) and bold_px (text contract) are compared with Python. M7: the checks no other C++ test made are in test_ledger_service_c.cpp"
 },
 "tests/test_render_clip.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_render.cpp",
   "tools/migration/render_harness.py",
   "native/tests/contract/test_contract_drawn_by_ops.cpp"
  ],
  "note": "Print clipping to panels and name-mode name strokes compared pixel for pixel in print/proof/name."
 },
 "tests/test_server.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_mcp.cpp",
   "tools/migration/make_mcp_cases.py",
   "tools/migration/mcp_harness.py"
  ],
  "note": "The MCP contract's api job runs genko serve against Python's: /health, /v1/new, /v1/apply with add_line, /v1/inspect and /v1/export of PNGs, replies compared."
 },
 "tests/test_story_and_export.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/lines_cases.json",
   "native/tests/unit/test_render_page.cpp",
   "native/tests/unit/test_ops.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_render.cpp"
  ],
  "note": "export_plan is only checked by count in test_render_page; draft/name left out of print and exports is checked by render/export parity. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_studio_flow.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_mcp.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_storage.cpp",
   "native/tests/e2e/test_mcp_api_e2e.cpp",
   "native/tests/unit/test_ledger_service_c.cpp",
   "native/tests/unit/test_service.cpp",
   "tools/migration/make_mcp_cases.py",
   "tools/migration/make_service_cases.py"
  ],
  "note": "Names, errors that block a commit (layout_ratio_sum at /tiers, page_not_blank), the agent's op refusals (approve, import_psd/put_raster/font_path outside, unlock_page), projects outside the root and the genko studio command line are compared with Python in the flow, cli and MCP api jobs. M7: the checks no other C++ test made are in test_ledger_service_c.cpp"
 },
 "tests/test_studio_mcp.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/e2e/test_mcp_api_e2e.cpp",
   "native/tests/contract/test_contract_mcp.cpp",
   "tools/migration/make_mcp_cases.py",
   "native/tests/contract/test_contract_pipeline.cpp",
   "native/tests/contract/test_contract_service.cpp"
  ],
  "note": "A person's actor refused by genko mcp (test_mcp_api_e2e); the name flow, tool list, resources (manga-rules, ops), images and stdio in the MCP contract's client and stdio jobs; the art flow over MCP (import, adopt, panel renders, approve refused) in the pipeline run and the service art job."
 },
 "tests/test_studio_units.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/unit/test_ledger_service_c.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/studio_harness.py"
  ],
  "note": "Schemas and validation paths (studioFilesLikePython), tategaki columns with newlines (text contract), and the demo plans laid out and lettered by submit_name (flow job, replies and previews byte for byte) are covered. M7: the checks no other C++ test made are in test_ledger_service_c.cpp"
 },
 "tests/test_t_report.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/gui/test_gui_look.cpp",
   "native/tests/unit/test_ledger_service_c.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/pyref_harness.py"
  ],
  "note": "Lines following a moved gutter (ops_cases on page 1's lines), the gutters handed back by inspect page and spec presets (flow job), unknown plan keys and their hint (studioFilesLikePython) and the graphics card off until chosen (test_gui_look) are covered. M7: the checks no other C++ test made are in test_ledger_service_c.cpp"
 },
 "tests/test_tategaki.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_text.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "Vertical forms (choonpu, kuten, small kana, brackets) compared glyph by glyph with Python. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_tibo_report.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_checks.cpp",
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/contract/test_contract_studio.cpp",
   "native/tests/gui/test_gui_book.cpp",
   "native/tests/gui/test_gui_studio.cpp",
   "native/tests/unit/test_ledger_service_c.cpp",
   "tools/migration/make_service_cases.py",
   "tools/migration/make_studio_cases.py",
   "tools/migration/render_harness.py",
   "tools/migration/studio_harness.py"
  ],
  "note": "The style lock made again and going stale (studio cases), art replaced sending the page back to ink with stale faces (studio cases, checks), withdrawing own sheet candidates, the same picture imported again, and same-named styles with their genres (style job, test_gui_studio) are covered. M7: the checks no other C++ test made are in test_ledger_service_c.cpp"
 },
 "tests/test_ui_look.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/gui/test_gui_look.cpp",
   "native/tests/gui/test_gui_output.cpp",
   "native/tests/gui/test_gui_studio.cpp"
  ],
  "note": "test_gui_look ports most functions one-to-one; the approval box coming forward is test_gui_studio requestsArrive; phone screens are test_gui_output phoneScreens. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_ux.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_service.cpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_canvas.cpp",
   "native/tests/gui/test_gui_layout.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/gui/test_gui_look.cpp",
   "native/tests/gui/test_gui_output.cpp",
   "native/tests/gui/test_gui_studio.cpp",
   "native/tests/unit/test_service.cpp",
   "native/tests/unit/test_session.cpp",
   "tools/migration/make_service_cases.py"
  ],
  "note": "Redo, every export format from the dialog = genko export, export fields, approval box/viewer, sheets, delete-page question, review.html (service contract) and `studio approve` without --as are covered. Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_ux_review.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_studio_gui.cpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_ai.cpp",
   "native/tests/gui/test_gui_layout.cpp",
   "native/tests/gui/test_gui_ledger_b.cpp",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/gui/test_gui_look.cpp",
   "native/tests/gui/test_gui_workspace.cpp"
  ],
  "note": "Tooltips with current keys, short-screen palette, first steps, ai_link texts (contract), AI dialog, panel reference and 〔kind〕 rows are covered. M7: the checks no other C++ test made are in test_gui_ledger_b.cpp"
 },
 "tests/test_w10_rulers.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/contract/test_contract_rulers.cpp",
   "native/tests/gui/test_gui_guides.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/unit/test_rulers.cpp",
   "tools/migration/tone_harness.py"
  ],
  "note": "Rect/ellipse/polygon snapping, outlines, perspective grids and ruler_from_3d/camera_from_ruler are covered (unit test_rulers carries these Python cases; exact numbers in the rulers and 3D contracts). M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_w11_effects.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_tone_render.cpp",
   "tools/migration/tone_harness.py",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/tone_cases.hpp",
   "native/tests/unit/test_tones_effects.cpp",
   "native/tests/gui/test_gui_effects.cpp"
  ],
  "note": "Bundle/jitter/taper/spacing/length geometry is checked by pixel-exact parity with Python on random books using those params (not by property asserts); refusals in unit + fixed cases; the effect panel's bundle field is only checked to exist, taper none->false and length 0->null are checked. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_w12_figure.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/test_contract_3d_geometry.cpp",
   "native/tests/contract/test_contract_3d_ops.cpp",
   "native/tests/gui/test_gui_guides.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/unit/test_ops_3d.cpp",
   "tools/migration/geom3d_harness.py"
  ],
  "note": "IK drags, builds, finger curls, poses library, spheres/cones/props are compared bit for bit with Python (ops cases + random geometry). Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_w13_scan.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json",
   "tools/migration/pyref_harness.py",
   "native/tests/gui/test_gui_effects.cpp"
  ],
  "note": "lineart settings (drop_blue, min_px, keep_solid, rgb, threshold) by pixel parity on a picture with a blue ellipse and black solids; scanner commands/errors and scan import in test_gui_effects. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_w14_speed.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/gui/test_gui_look.cpp",
   "native/tests/gui/test_gui_brushes.cpp",
   "native/tests/gui/test_gui_layers.cpp"
  ],
  "note": "aToolsKeyHeldIsThatToolOnlyWhileHeld, eachToolHasItsOwnModifierKeys, nothingWatchesEveryEventOfTheAppUnlessNeeded, sizePresets, ownBrushes (edited again), showHideAndAlone (solo); the size grid's '＋' label is not checked."
 },
 "tests/test_w15_color.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/book_cases.json",
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_book.cpp",
   "native/tests/contract/test_contract_filters.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/gui/test_gui_look.cpp"
  ],
  "note": "Gradients (stops, ellipse, repeat/mirror, refusals), gradient map/glow/rain, import_pages, backups and the obi are covered. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_w1_panels.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_book.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_render.cpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/unit/test_model.cpp",
   "tools/migration/render_harness.py"
  ],
  "note": "panel_each/rounded panels pixel-compared, add_frame/delete_frame/freehand/too-small/last-panel and set_page_spec moving drawn panels compared with Python. Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_w1_panels_gui.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_brushes.cpp",
   "native/tests/gui/test_gui_canvas.cpp",
   "native/tests/gui/test_gui_guides.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp"
  ],
  "note": "Ruler → cut_frame/add_frame (test_gui_guides), panel → selection and own layouts first (test_gui_actions). M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_w2_lines.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_brushes.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/gui/test_gui_brushes.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/unit/test_stroke_geom.cpp",
   "tools/migration/pyref_harness.py",
   "tools/migration/render_harness.py"
  ],
  "note": "taper_points/stabilize_points/fit_curve are compared bit for bit against a Python table; pressure_opacity drawing by brush parity; the op fields and save/reload by ops cases. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_w3_fill.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/test_contract_raster_render.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/gui/test_gui_paint.cpp",
   "native/tests/unit/test_raster_ops.cpp",
   "tools/migration/pyref_harness.py"
  ],
  "note": "fill_enclosed (inside the lasso, nothing closed refused), ignore draft/text and tolerance cases compared pixel for pixel with Python. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_w4_mend.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_vector.cpp",
   "native/tests/contract/vector_cases.json",
   "native/tests/gui/test_gui_paint.cpp"
  ],
  "note": "trace_edit widen/narrow/redraw/redraw_width/join, set_pressure and simplify with refusals compared with Python; the GUI pinch with pinned ends asserts one end kept. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_w5_snap_erase.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/ops_cases.json",
   "tools/migration/pyref_harness.py",
   "native/tests/contract/test_contract_rulers.cpp",
   "native/tests/contract/ruler_cases.json",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/contract/raster_cases.json"
  ],
  "note": "snap_lines_mm (fixed + random), erase along a ruler, soft/rough/hard erase and texture refusal compared with Python. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_w6_balloons.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_balloons.cpp",
   "tools/migration/render_harness.py",
   "native/tests/contract/test_contract_text.cpp",
   "native/tests/contract/test_contract_lines.cpp",
   "native/tests/contract/lines_cases.json",
   "native/tests/gui/test_gui_lines.cpp"
  ],
  "note": "Balloon colours, text offsets, bent tails, cuts, hand outlines (_smooth_closed bit for bit) compared with Python; the tail bend in test_gui_lines. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_w6_lettering.py": {
  "verdict": "covered",
  "cpp": [
   "native/tests/contract/test_contract_text.cpp",
   "tools/migration/render_harness.py",
   "native/tests/gui/test_gui_lines.cpp",
   "native/tests/contract/test_contract_render.cpp"
  ],
  "note": "mark_tcy/mono_runs/compose styles compared with Python; parse_marks/with_marks in test_gui_lines; below_layer by render parity. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite."
 },
 "tests/test_w7_tone.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/ops_cases.json",
   "native/tests/contract/test_contract_export.cpp",
   "native/tests/contract/test_contract_ops.cpp",
   "native/tests/contract/test_contract_tone_ops.cpp",
   "native/tests/contract/test_contract_tone_render.cpp",
   "native/tests/contract/tone_cases.hpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/unit/test_tones_effects.cpp",
   "tools/migration/tone_harness.py"
  ],
  "note": "threshold tiles (unit), tone dot shapes/offsets/move back (unit + fixed cases + pixel parity), bitonal toning and export screen refusal covered. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_w8_filters.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_filters.cpp",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/gui/test_gui_layers.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/unit/test_color_filters.cpp",
   "tools/migration/pyref_harness.py",
   "tools/migration/render_harness.py"
  ],
  "note": "Adjustment tables, despeckle, filter in an area, one-point curve refusal compared with Python; the page preview while the filter dialog is open is checked in test_gui_layers. Contract tests QSKIP without the reference Python (no GENKO_PYREF or /opt/pyref in this shell) but run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 },
 "tests/test_w9_view.py": {
  "verdict": "ported",
  "cpp": [
   "native/tests/contract/raster_cases.json",
   "native/tests/contract/test_contract_raster_ops.cpp",
   "native/tests/gui/test_gui_actions.cpp",
   "native/tests/gui/test_gui_effects.cpp",
   "native/tests/gui/test_gui_ledger_c.cpp",
   "native/tests/gui/test_gui_select.cpp",
   "native/tests/unit/test_raster_ops.cpp"
  ],
  "note": "Mesh warps with a grid and wrong counts are contract cases; transform_matrix about the pivot is checked in test_gui_select. Contract tests QSKIP without the reference Python ($GENKO_PYREF or /opt/pyref, absent in this shell); they run in the full suite. M7: the checks no other C++ test made are in test_gui_ledger_c.cpp"
 }
}


class _Tree:
    def __init__(self, roots, suffixes):
        self.texts: dict[Path, str] = {}
        for root in roots:
            for path in sorted(root.rglob("*")):
                if path.is_file() and path.suffix in suffixes:
                    self.texts[path] = path.read_text(encoding="utf-8", errors="replace")
        self.strings: dict[str, str] = {}  # every string literal → where it first appears
        for path, text in self.texts.items():
            quotes = r'"((?:[^"\\\n]|\\.)*)"' + (r"|'((?:[^'\\\n]|\\.)*)'" if path.suffix == ".py" else "")
            for m in re.finditer(quotes, text):
                self.strings.setdefault(m.group(1) if m.group(1) is not None else m.group(2), self.where(path, m.start()))

    def where(self, path: Path, at: int) -> str:
        return f"{path.relative_to(ROOT).as_posix()}:{self.texts[path].count(chr(10), 0, at) + 1}"

    def first(self, pattern: str, only: str = "") -> str | None:
        rx = re.compile(pattern)
        for path, text in self.texts.items():
            if only and not path.relative_to(ROOT).as_posix().startswith(only):
                continue
            m = rx.search(text)
            if m:
                return self.where(path, m.start())
        return None


def _registered_tests() -> set[str]:
    return set(re.findall(r"genko_test\(\s*(\w+)", (NATIVE / "tests" / "CMakeLists.txt").read_text(encoding="utf-8")))


def _cpp_found(row: dict, src: _Tree) -> str | None:
    kind, name = row["kind"], row["name"]
    q = re.escape(name)
    if kind == "op":
        if name in BUS_OPS:
            return src.first(r'is_op\([^()]*"%s"\)' % q, "native/src/core/command_bus.cpp")
        return src.first(r'registry\.add\("%s"' % q) or src.first(
            r'for \(const char\* name : \{[^}]*"%s"[^}]*\}\)\s*\{?\s*registry\.add\(name' % q)
    if kind == "mcp_tool":
        return src.first(r'\{"%s",\s*R"\(' % q, "native/src/api/mcp_tools.cpp")
    if kind == "mcp_resource":
        return src.first(r'\{"%s",' % q, "native/src/api/")
    if kind == "cli":
        words = name.split()[1:]
        if words[0] == "studio" and len(words) == 2:
            return src.first(r'"%s"' % re.escape(words[1]), "native/src/api/studio_cli.cpp")
        w = re.escape(words[0])
        return src.first(r'command == QLatin1String\("%s"\)|== "%s"\)' % (w, w), "native/src/api/")
    if kind == "http":
        if name.endswith("*"):
            return src.first(r'route\.rfind\("%s", 0\) == 0' % re.escape(name[:-1]), "native/src/api/headless_server.cpp")
        return src.first(r'route == "%s"' % q, "native/src/api/headless_server.cpp")
    if kind == "export_format":
        return src.first(r'f == "%s"' % q, "native/src/api/cli.cpp") or src.first(r'\{"%s", "' % q, "native/src/formats/exporting.cpp")
    if kind == "gui_action":  # (made with its title: make, tool, toggle …)
        return src.first(r'"%s",\s*(QStringLiteral|QString::fromUtf8)\(' % re.escape(row["attribute"]), "native/src/app/")
    if kind == "gui_action_group":
        return src.first(r"\b%s_;" % q, "native/src/app/main_window.hpp")
    return None


def _test_found(row: dict, tests: _Tree) -> str | None:
    kind, name = row["kind"], row["name"]
    if kind == "cli":
        return tests.strings.get(name.split()[-1])
    if kind == "http":
        return tests.first(re.escape(name.rstrip("*")))
    if kind == "gui_action":
        return tests.strings.get(row["attribute"])
    if kind == "gui_action_group":  # (by the window's list, or a member picked by its words and triggered)
        return tests.first(GROUP_TESTS.get(name, r"\b%s\(\)" % re.escape(name)), "native/tests/")
    return tests.strings.get(name)


def _judged(row: dict, entry: dict | None, registered: set[str]) -> None:
    if entry is None:
        row["status"] = "pending"
        row["missing"] = ["not judged"]
        return
    files = sorted(set(entry.get("cpp", []) + entry.get("tests", [])))
    gone = [f for f in files if not (ROOT / f).is_file()]
    unregistered = [f for f in files if f.startswith("native/tests/") and f.endswith(".cpp") and "/support/" not in f
                    and Path(f).stem not in registered]
    row["cpp"] = files
    for key in ("missing", "note"):
        if entry.get(key):
            row[key] = entry[key]
    if gone or unregistered:
        row["status"] = "pending"
        row["missing"] = row.get("missing", []) + [f"no such file: {f}" for f in gone] + \
            [f"test not registered: {f}" for f in unregistered]
        return
    row["status"] = {"ported": "ported", "covered": "ported", "partial": "in_progress",
                     "not_applicable": "not_applicable"}.get(entry["verdict"], "pending")


def cpp_status(rows: list[dict]) -> None:
    src = _Tree([NATIVE / "src"], {".cpp", ".hpp", ".in"})
    tests = _Tree(TEST_TREES, {".cpp", ".hpp", ".py", ".json", ".txt"})
    registered = _registered_tests()
    for row in rows:
        if row["kind"] == "python_module":
            _judged(row, MODULE_CPP.get(row["name"]), registered)
            continue
        if row["kind"] == "test_file":
            _judged(row, TEST_CPP.get(row["name"]), registered)
            continue
        found, tested = _cpp_found(row, src), _test_found(row, tests)
        if found:
            row["cpp"] = found
        if tested:
            row["cpp_test"] = tested
        row["status"] = "ported" if found and tested else "pending"
        if row["status"] == "pending":
            row["missing"] = (["C++ source"] if not found else []) + (["C++ test"] if not tested else [])


def accept(rows: list[dict], folder: Path) -> str:
    audit_file = folder / "matrix-audit.json"
    audit = json.loads(audit_file.read_text(encoding="utf-8")) if audit_file.is_file() else {}
    if not audit.get("passed"):
        return f"not accepted: {audit_file} is missing or did not pass ({audit.get('failures')})"
    for row in rows:
        if row["status"] == "ported":
            row["status"] = "accepted"
    return f"accepted by {audit_file} ({', '.join(audit.get('presets', []))})"


def build() -> dict:
    inv = json.loads((DOCS / "baseline-inventory.json").read_text(encoding="utf-8"))
    if inv["baseline_commit"] != BASELINE:
        raise SystemExit("baseline-inventory.json is for another commit")
    rows: list[dict] = []
    unassigned: list[str] = []
    r, u = ops_rows(inv["ops_schema_names"]); rows += r; unassigned += u
    rows += mcp_rows(inv["mcp_tools"], inv["mcp_resources"])
    r, u = cli_rows(inv["cli_literal_commands"]); rows += r; unassigned += u
    rows += http_rows()
    rows += export_rows(inv["cli_export_choices"])
    rows += gui_rows()
    r, u = module_rows(inv["python_modules"]); rows += r; unassigned += u
    r, u = test_rows(); rows += r; unassigned += u
    cpp_status(rows)
    counts: dict[str, dict[str, int]] = {}
    for row in rows:
        counts.setdefault(row["kind"], {}).setdefault(row["milestone"], 0)
        counts[row["kind"]][row["milestone"]] += 1
    return {"baseline_commit": BASELINE, "method": "static extraction; see tools/migration/build_ledger.py",
            "statuses": ["pending", "in_progress", "ported", "accepted", "blocked", "not_applicable"],
            "counts": counts, "total": len(rows), "unassigned": unassigned, "rows": rows}


def status_counts(ledger: dict) -> dict[str, dict[str, int]]:
    out: dict[str, dict[str, int]] = {}
    for row in ledger["rows"]:
        out.setdefault(row["kind"], {}).setdefault(row["status"], 0)
        out[row["kind"]][row["status"]] += 1
    return out


def markdown(ledger: dict) -> str:
    lines = ["# 移行台帳（自動生成）", "",
             f"基準コミット `{ledger['baseline_commit']}`。`tools/migration/build_ledger.py` が生成する。手で編集しない。", "",
             f"行数: {ledger['total']}。未割当: {len(ledger['unassigned'])}。", "",
             "| 種別 | " + " | ".join(f"M{i}" for i in range(1, 7)) + " | 計 |", "|---|" + "---|" * 7]
    for kind, by in sorted(ledger["counts"].items()):
        cells = [str(by.get(f"M{i}", 0)) for i in range(1, 7)]
        lines.append(f"| {kind} | " + " | ".join(cells) + f" | {sum(by.values())} |")
    statuses = ledger["statuses"]
    lines += ["", "## 状態（C++ の木との突き合わせ）", "",
              "ported は C++ のソースにあり C++ の試験（両方の版で流す契約試験の事例を含む）が指すもの、in_progress は一部だけ"
              "（足りないものを missing に）、not_applicable は Python だけの仕組み（理由を note に）、pending は見つからないもの。"
              "accepted は計画のすべての構成（Windows を含む）の `--pair` が合格したときだけ `--accepted` で付く。",
              "", "| 種別 | " + " | ".join(statuses) + " |", "|---|" + "---|" * len(statuses)]
    for kind, by in sorted(status_counts(ledger).items()):
        lines.append(f"| {kind} | " + " | ".join(str(by.get(s, 0)) for s in statuses) + " |")
    rest = [r for r in ledger["rows"] if r["status"] not in ("ported", "accepted")]
    if rest:
        lines += ["", "### ported でない行", ""]
        for r in rest:
            why = "; ".join(r.get("missing", [])) or r.get("note", "")
            lines.append(f"- `{r['id']}`（{r['status']}）: {why}")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ledger = build()
    note = ""
    if "--accepted" in sys.argv:
        note = accept(ledger["rows"], Path(sys.argv[sys.argv.index("--accepted") + 1]))
    text = json.dumps(ledger, ensure_ascii=False, indent=1) + "\n"
    md = markdown(ledger)
    if "--check" in sys.argv:
        current = (DOCS / "ledger.json").read_text(encoding="utf-8") if (DOCS / "ledger.json").is_file() else ""
        old = json.loads(current) if current else {}
        same = old.get("total") == ledger["total"] and [r["id"] for r in old.get("rows", [])] == [r["id"] for r in ledger["rows"]]
        # (the committed status may be "accepted" where the tree alone says "ported")
        moved = [r["id"] for r, o in zip(ledger["rows"], old.get("rows", []))
                 if r["status"] != o.get("status") and not (r["status"] == "ported" and o.get("status") == "accepted")]
        print(json.dumps({"same_rows": same, "status_differs": moved[:20], "unassigned": ledger["unassigned"]},
                         ensure_ascii=False))
        return 0 if same and not moved and not ledger["unassigned"] else 1
    (DOCS / "ledger.json").write_text(text, encoding="utf-8")
    (DOCS / "LEDGER.md").write_text(md, encoding="utf-8")
    print(json.dumps({"total": ledger["total"], "status": status_counts(ledger), "unassigned": ledger["unassigned"],
                      "accepted": note}, ensure_ascii=False))
    return 1 if ledger["unassigned"] else 0


if __name__ == "__main__":
    sys.exit(main())
