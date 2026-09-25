"""G6: agents can do what people do — pages, layers, masks, brushes, pixels, gradients, the lettering
materials — plus the same check, their own undo and every export; approvals stay with people."""

import json
from pathlib import Path

import pytest

from genko.io import load_episode, save_episode
from genko.models import LayerRole
from genko.ops import apply_ops
from genko.studio.service import AGENT_OPS, StudioService

GUIDE = Path(__file__).resolve().parents[1] / "src" / "genko" / "studio" / "guide" / "SKILL.md"
AGENT_DOC = Path(__file__).resolve().parents[1] / "docs" / "AGENT.md"


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))


@pytest.fixture
def agent(tmp_path: Path):
    service = StudioService(tmp_path, "ai:hermes")
    assert service.create_project("demo.genko", "demo", 4).ok
    path = service.project_path("demo.genko")
    episode = load_episode(path)
    apply_ops(episode, [{"op": "name_ok", "page": 1}], agent="human:leaf")  # a person approved page 1's name
    save_episode(episode, path, actor="human:leaf")
    return service


def _ok(result):
    assert result.ok, result.to_dict()
    return result.data


def test_the_ops_people_use_are_open_to_agents_and_approvals_are_not():
    for name in ("add_page", "delete_page", "duplicate_page", "reorder", "set_spread", "set_page_spec", "delete_layer",
                 "duplicate_layer", "merge_down", "set_layer_mask", "paint_mask", "define_brush", "filter_raster", "flood_fill",
                 "erase_raster", "gradient_fill", "set_onion", "step_onion"):
        assert name in AGENT_OPS, name
    for name in ("approve", "revoke", "name_ok", "advance", "lock_page", "unlock_page", "reject_sheet", "resolve_proposal",
                 "put_raster"):  # (put_raster can read local files: agents use import_image)
        assert name not in AGENT_OPS, name


def test_an_agent_draws_with_everything(agent: StudioService):
    ops = [
        {"op": "add_layer", "page": 1, "kind": "paint", "id": "p", "name": "塗り"},
        {"op": "define_brush", "key": "my_dry", "label": "かすれ", "base": "fude", "texture": "dry"},
        {"op": "add_stroke", "page": 1, "layer_id": "p", "kind": "my_dry", "points": [[40, 60], [120, 90]], "width_mm": 2},
        {"op": "gradient_fill", "page": 1, "layer_id": "p", "from": [40, 200], "to": [200, 200], "rgb_from": [0, 0, 0]},
        {"op": "duplicate_layer", "page": 1, "id": "p", "new_id": "p2"},
        {"op": "set_layer_mask", "page": 1, "id": "p2", "area": {"poly": [[0, 0], [100, 0], [100, 100], [0, 100]]}},
        {"op": "paint_mask", "page": 1, "id": "p2", "points": [[150, 150], [160, 160]], "show": True},
        {"op": "merge_down", "page": 1, "id": "p2"},
        {"op": "filter_raster", "page": 1, "id": "p", "kind": "curve", "gamma": 1.4},
        {"op": "transform_area", "page": 1, "layer_id": "p", "area": {"poly": [[30, 50], [130, 50], [130, 100], [30, 100]]},
         "warp": {"perspective": [[35, 45], [125, 55], [135, 105], [25, 95]]}},
        {"op": "add_line", "page": 1, "id": "l", "text": "本当か", "x_mm": 150, "y_mm": 40, "w_mm": 30, "h_mm": 50, "wrap": "vertical",
         "emphasis_runs": ["本当"], "style_runs": [["か", {"scale": 1.4}]], "style": {"rotate_deg": 10, "wobble": 0.4}},
        {"op": "add_prim3d", "page": 1, "kind": "stairs", "pos": [100, 250, 0]},
        {"op": "add_page", "after": 4},
        {"op": "duplicate_page", "page": 2},
        {"op": "set_page_spec", "preset": "b5"},
    ]
    data = _ok(agent.apply_ops("demo.genko", ops, commit=True))
    assert data["committed"]
    episode = load_episode(agent.project_path("demo.genko"))
    assert len(episode.pages) == 6 and episode.brush_custom["my_dry"]["label"] == "かすれ"
    assert episode.story[0].style_runs == [["か", {"scale": 1.4}]]
    # the approved-name rule still holds for pages whose name is not approved
    refused = agent.apply_ops("demo.genko", [{"op": "gradient_fill", "page": 2, "layer": "ink", "from": [1, 1], "to": [90, 90],
                                              "layer_id": next(x.id for x in episode.pages[1].layers if x.role == LayerRole.INK)}])
    assert not refused.ok
    # approvals stay with people
    assert not agent.apply_ops("demo.genko", [{"op": "name_ok", "page": 2}], commit=True).ok


def test_check_undo_and_export(agent: StudioService, tmp_path: Path):
    _ok(agent.apply_ops("demo.genko", [{"op": "add_line", "page": 1, "text": "はみ出し", "x_mm": 250, "y_mm": 40, "w_mm": 20, "h_mm": 30}],
                        commit=True))
    found = _ok(agent.check("demo.genko"))
    assert found["errors"] >= 1 and any(i["page"] == 1 for i in found["issues"])
    from genko import checks

    assert found["issues"] == checks.book(load_episode(agent.project_path("demo.genko")), agent.project_path("demo.genko"))["issues"]
    assert "checks" in _ok(agent.preflight("demo.genko"))
    # undo: its own change only
    _ok(agent.undo("demo.genko"))
    assert not load_episode(agent.project_path("demo.genko")).story
    episode = load_episode(agent.project_path("demo.genko"))
    apply_ops(episode, [{"op": "set_note", "page": 1, "note": "人のメモ"}], agent="human:leaf")
    save_episode(episode, agent.project_path("demo.genko"), actor="human:leaf")
    assert not agent.undo("demo.genko").ok  # a person's change
    # every format, and page ranges
    for fmt in ("png", "psd", "strip", "sns"):
        data = _ok(agent.export("demo.genko", fmt, pages=[1, 2], dpi=40))
        assert data["files"] and Path(data["folder"]).is_dir() and "exports" in data["folder"]
    assert len(_ok(agent.export("demo.genko", "png", pages=[2], dpi=40))["files"]) == 1
    assert not agent.export("demo.genko", "png", pages=[9]).ok
    assert not agent.export("demo.genko", "gif").ok


def test_the_mcp_server_offers_the_tools_and_the_guides_tell_of_them(tmp_path: Path):
    pytest.importorskip("mcp")
    import anyio
    from mcp import Client

    from genko.mcp.server import build_server

    server = build_server(tmp_path, "ai:hermes")

    async def scenario():
        async with Client(server) as client:
            tools = {t.name for t in (await client.list_tools()).tools}
            assert {"check", "undo", "export"} <= tools
            assert json.loads((await client.call_tool("create_project", {"name": "d.genko", "title": "d", "pages": 2})).content[0].text)["ok"]
            result = json.loads((await client.call_tool("export", {"project": "d.genko", "format": "png", "dpi": 30})).content[0].text)
            assert result["ok"] and len(result["files"]) == 2

    anyio.run(scenario)
    guide = GUIDE.read_text(encoding="utf-8")
    for word in ("emphasis_runs", "style_runs", "rotate_deg", "set_layer_mask", "merge_down", "warp", "define_brush",
                 "gradient_fill", "mcp_genko_check", "mcp_genko_undo", "mcp_genko_export", "cylinder", "wobble"):
        assert word in guide, word
    doc = AGENT_DOC.read_text(encoding="utf-8")
    assert "Parity with the app" in doc and "gradient_fill" in doc
