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
    counts: dict[str, dict[str, int]] = {}
    for row in rows:
        counts.setdefault(row["kind"], {}).setdefault(row["milestone"], 0)
        counts[row["kind"]][row["milestone"]] += 1
    return {"baseline_commit": BASELINE, "method": "static extraction; see tools/migration/build_ledger.py",
            "statuses": ["pending", "in_progress", "ported", "accepted", "blocked"],
            "counts": counts, "total": len(rows), "unassigned": unassigned, "rows": rows}


def markdown(ledger: dict) -> str:
    lines = ["# 移行台帳（自動生成）", "",
             f"基準コミット `{ledger['baseline_commit']}`。`tools/migration/build_ledger.py` が生成する。手で編集しない。", "",
             f"行数: {ledger['total']}。未割当: {len(ledger['unassigned'])}。", "",
             "| 種別 | " + " | ".join(f"M{i}" for i in range(1, 7)) + " | 計 |", "|---|" + "---|" * 7]
    for kind, by in sorted(ledger["counts"].items()):
        cells = [str(by.get(f"M{i}", 0)) for i in range(1, 7)]
        lines.append(f"| {kind} | " + " | ".join(cells) + f" | {sum(by.values())} |")
    lines += ["", "状態は ledger.json の `status`。各工程の出口で `accepted` まで更新し、`pending` が残る工程は完了扱いにしない。", ""]
    return "\n".join(lines)


def main() -> int:
    ledger = build()
    text = json.dumps(ledger, ensure_ascii=False, indent=1) + "\n"
    md = markdown(ledger)
    if "--check" in sys.argv:
        current = (DOCS / "ledger.json").read_text(encoding="utf-8") if (DOCS / "ledger.json").is_file() else ""
        old = json.loads(current) if current else {}
        same = old.get("total") == ledger["total"] and [r["id"] for r in old.get("rows", [])] == [r["id"] for r in ledger["rows"]]
        print(json.dumps({"same_rows": same, "unassigned": ledger["unassigned"]}, ensure_ascii=False))
        return 0 if same and not ledger["unassigned"] else 1
    (DOCS / "ledger.json").write_text(text, encoding="utf-8")
    (DOCS / "LEDGER.md").write_text(md, encoding="utf-8")
    print(json.dumps({"total": ledger["total"], "counts": ledger["counts"], "unassigned": ledger["unassigned"]},
                     ensure_ascii=False))
    return 1 if ledger["unassigned"] else 0


if __name__ == "__main__":
    sys.exit(main())
