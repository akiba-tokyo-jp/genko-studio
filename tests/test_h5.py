"""H5: agents can list materials, fonts and brushes, read every layer's and line's settings, and look
at one layer alone or the page as it prints — and the guides say so."""

import inspect as pyinspect
from io import BytesIO
from pathlib import Path

import pytest
from PIL import Image

from genko.io import load_episode, save_episode
from genko.models import LayerRole
from genko.ops import apply_ops
from genko.studio.service import AGENT_OPS, StudioService

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))


@pytest.fixture
def agent(tmp_path: Path):
    service = StudioService(tmp_path, "ai:hermes")
    assert service.create_project("demo.genko", "demo", 2).ok
    path = service.project_path("demo.genko")
    episode = load_episode(path)
    apply_ops(episode, [{"op": "name_ok", "page": 1}], agent="human:leaf")
    save_episode(episode, path, actor="human:leaf")
    return service


def _ok(result):
    assert result.ok, result.to_dict()
    return result.data


def test_materials_fonts_and_brushes_can_be_listed(agent: StudioService):
    materials = _ok(agent.inspect("demo.genko", "materials"))["materials"]
    assert materials and all({"id", "name", "kind", "folder"} <= set(m) for m in materials)
    tone = next(m for m in materials if m["kind"] == "tone")
    # what is listed can be placed
    frame = load_episode(agent.project_path("demo.genko")).pages[0].leaf_frames()[0].id
    _ok(agent.apply_ops("demo.genko", [{"op": "stamp_material", "page": 1, "material_id": tone["id"], "frame_id": frame}], commit=True))
    fonts = _ok(agent.inspect("demo.genko", "fonts"))["fonts"]
    assert {f["key"] for f in fonts["bundled"]} >= {"antique", "gothic", "mincho", "sfx"} and "system" in fonts
    brushes = _ok(agent.inspect("demo.genko", "brushes"))["brushes"]
    assert {"gpen", "maru", "fude"} <= {b["key"] for b in brushes["builtin"]}
    _ok(agent.apply_ops("demo.genko", [{"op": "define_brush", "key": "my_dry", "label": "かすれ", "base": "fude"}], commit=True))
    brushes = _ok(agent.inspect("demo.genko", "brushes"))["brushes"]
    assert any(b["key"] == "my_dry" and b.get("label") == "かすれ" for b in brushes["book"])
    assert not agent.inspect("demo.genko", "castle").ok


def test_the_snapshot_tells_layers_and_lines_in_full(agent: StudioService):
    ops = [{"op": "add_layer", "page": 1, "kind": "folder", "id": "f", "name": "背景"},
           {"op": "add_layer", "page": 1, "kind": "paint", "id": "p", "name": "影", "blend": "multiply", "parent": "f"},
           {"op": "set_layer", "page": 1, "id": "p", "opacity": 0.4, "color": [40, 110, 230], "reference": True},
           {"op": "set_layer_mask", "page": 1, "id": "p", "fill": "show"},
           {"op": "add_line", "page": 1, "text": "もしもし", "balloon": "electric", "x_mm": 40, "y_mm": 40, "w_mm": 30, "h_mm": 40,
            "style": {"weight": "heavy"}},
           {"op": "add_scene", "page": 1, "kind": "classroom", "id": "s"}]
    _ok(agent.apply_ops("demo.genko", ops, commit=True))
    snap = _ok(agent.inspect("demo.genko", "snapshot"))["snapshot"]
    page = snap["pages"][0]
    layer = next(item for item in page["layers"] if item["id"] == "p")
    assert layer["title"] == "影" and layer["kind"] in ("paint", "raster")
    assert layer["opacity"] == 0.4 and layer["blend"] == "multiply" and layer["parent_id"] == "f"
    assert layer["color"] == [40, 110, 230] and layer["reference"] is True and layer["mask"] == {"enabled": True}
    line = page["story"][0]
    assert line["balloon"] == "electric" and line["style"]["weight"] == "heavy" and line["wrap"] and line["w_mm"] == 30
    assert {"id": "s", "kind": "scene", "scene": "classroom"} in page["prims"]


def test_render_one_layer_or_the_print_look(agent: StudioService):
    path = agent.project_path("demo.genko")
    ink = next(layer for layer in load_episode(path).pages[0].layers if layer.role == LayerRole.INK).id
    _ok(agent.apply_ops("demo.genko", [
        {"op": "add_stroke", "page": 1, "layer_id": ink, "points": [[40, 60], [200, 60]], "width_mm": 3},
        {"op": "add_line", "page": 1, "text": "やあ", "x_mm": 150, "y_mm": 150, "w_mm": 30, "h_mm": 40},
    ], commit=True))
    alone = agent.render("demo.genko", 1, "proof", 600, layer_id=ink)
    assert alone.ok and alone.data["layer_id"] == ink
    image = Image.open(BytesIO(alone.images[0])).convert("L")
    assert image.getextrema()[0] < 80  # the line is there
    whole = Image.open(BytesIO(agent.render("demo.genko", 1, "proof", 600).images[0])).convert("L")
    assert image.tobytes() != whole.tobytes()  # (no frames, no lettering: the layer alone)
    printed = agent.render("demo.genko", 1, "print", 600)
    assert printed.ok and printed.data["mode"] == "print"
    assert not agent.render("demo.genko", 1, "proof", 600, layer_id="nope").ok
    assert not agent.render("demo.genko", 1, "sepia", 600).ok


def test_the_mcp_tools_take_the_new_arguments():
    from genko.mcp import server

    source = pyinspect.getsource(server)
    assert "layer_id: str | None = None" in source and "materials" in source and "brushes" in source
    assert "add_scene" in AGENT_OPS


def test_the_guides_tell_agents_about_it():
    skill = (ROOT / "src" / "genko" / "studio" / "guide" / "SKILL.md").read_text(encoding="utf-8")
    for words in ("materials", "fonts", "brushes", "layer_id", "mode: print", "add_scene", "electric", "weight", "reference"):
        assert words in skill, words
    agent_doc = (ROOT / "docs" / "AGENT.md").read_text(encoding="utf-8")
    assert "H5 additions" in agent_doc and "add_scene" in agent_doc
    claude = (ROOT / "integrations" / "claude-code" / "skills" / "genko-manga" / "SKILL.md").read_text(encoding="utf-8")
    assert claude == skill.replace("mcp_genko_", "mcp__genko__")
