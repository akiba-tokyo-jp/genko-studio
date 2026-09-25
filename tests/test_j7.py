"""J7: materials of every kind (marks, props, traced backgrounds, brushes, 3D, lettering), searched by name,
folder and tags, packs read from a folder or a zip and written out again."""

import json
import os
import sys
import zipfile
from pathlib import Path

import pytest
from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import save_episode  # noqa: E402
from genko.models import LayerRole, PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


def _ink(ep):
    return next(layer for layer in ep.pages[0].layers if layer.role == LayerRole.INK)


def test_there_are_many_kinds_of_materials():
    from collections import Counter

    from genko import materials

    kinds = Counter(item["kind"] for item in materials.all_materials())
    assert kinds["lines"] >= 30 and kinds["brush"] >= 6 and kinds["prim"] >= 9 and kinds["lettering"] >= 10
    assert len(materials.all_materials()) >= 80
    for item in materials.all_materials():
        assert materials.thumbnail(item, 40).size == (40, 40)
    folders = materials.folders()
    assert {"漫符", "小物", "背景（線画）", "ブラシ", "3D", "描き文字"} <= set(folders)


def test_search_finds_by_name_folder_tag_and_kind():
    from genko import materials

    assert [i["id"] for i in materials.search("汗")] == ["mark-汗"]
    assert {i["id"] for i in materials.search("教室")} >= {"bg-classroom", "3d-教室（3D）"}
    assert all(i["kind"] == "brush" for i in materials.search("", kind="brush"))
    assert materials.search("恋")[0]["id"] == "mark-ハート"  # (a tag)
    assert materials.search("ブラシ 雪")[0]["id"] == "brush-雪ブラシ"  # (every word)
    assert materials.search("そんなものはない") == []
    assert "漫符" in materials.all_tags()


def test_marks_props_and_backgrounds_go_on_a_layer():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ink = _ink(ep)
    for material in ("mark-汗", "prop-窓", "bg-room"):
        apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": material, "layer_id": ink.id, "x_mm": 100, "y_mm": 150}])
    assert len(_ink(ep).strokes) > 20


def test_a_brush_material_is_added_to_the_book_and_draws():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": "brush-星空ブラシ"}])
    (key,) = [k for k in ep.brush_custom]
    assert key.startswith("my_") and ep.brush_custom[key]["label"] == "星空"
    apply_ops(ep, [{"op": "add_stroke", "page": 1, "layer_id": _ink(ep).id, "points": [[40, 40], [120, 60]], "kind": key}])
    assert _ink(ep).strokes[-1].kind == key


def test_3d_materials_are_put_where_asked():
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": "3d-デッサン人形", "x_mm": 80, "y_mm": 120, "id": "m"},
                   {"op": "stamp_material", "page": 1, "material_id": "3d-箱", "x_mm": 150, "y_mm": 100, "id": "b"},
                   {"op": "stamp_material", "page": 1, "material_id": "3d-教室（3D）", "x_mm": 120, "y_mm": 200, "id": "s"}])
    prims = {p["id"]: p for p in ep.pages[0].prims}
    assert prims["m"]["kind"] == "figure" and prims["b"]["kind"] == "box" and prims["s"]["scene"] == "classroom"
    assert prims["b"]["pos"][:2] == [150.0, 100.0] and prims["s"]["pos"][:2] == [120.0, 200.0]


def _picture(path: Path, colour=(200, 30, 30, 255)) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    Image.new("RGBA", (20, 10), colour).save(path)
    return path


def test_packs_come_in_from_folders_and_zips_and_go_out(tmp_path):
    from genko import materials

    folder = tmp_path / "pack"
    _picture(folder / "a.png")
    _picture(folder / "木々" / "b.png")
    added = materials.import_pack(folder)
    assert {i["folder"] for i in added} == {"pack", "pack/木々"} and all(i["kind"] == "image" for i in added)
    materials.update_material(added[0]["id"], tags=["赤", "四角"])
    assert materials.search("四角")[0]["id"] == added[0]["id"]
    materials.add_material("マイ効果", "effect", "pack", effect="speed", params={"count": 12}, tags=["速い"])
    out = materials.export_pack([i["id"] for i in materials.user_materials()], tmp_path / "out.zip")
    with zipfile.ZipFile(out) as archive:
        entries = json.loads(archive.read("pack.json"))
        assert len(entries) == 3 and sum(1 for n in archive.namelist() if n.endswith(".png")) == 2
    for item in materials.user_materials():
        materials.delete_material(item["id"])
    again = materials.import_pack(out)
    assert len(again) == 3 and materials.search("速い")[0]["kind"] == "effect"
    assert materials.image_bytes(next(i for i in again if i["kind"] == "image"))
    with pytest.raises(ValueError):
        materials.import_pack(tmp_path / "nothing-here")
    empty = tmp_path / "empty"
    empty.mkdir()
    with pytest.raises(ValueError):
        materials.import_pack(empty)
    bad = tmp_path / "bad.zip"
    with zipfile.ZipFile(bad, "w") as archive:
        archive.writestr("../evil.png", b"x")
    with pytest.raises(ValueError):
        materials.import_pack(bad)


def test_agents_see_kinds_and_tags(tmp_path):
    from genko.studio.service import _materials_list

    listed = {m["id"]: m for m in _materials_list()}
    assert listed["mark-汗"]["tags"] and listed["sfx-ドーン"]["text"] == "ドーン" and listed["3d-箱"]["kind"] == "prim"
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "stamp_material", "page": 1, "material_id": "no-such"}])


# --- the app -----------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


@pytest.fixture
def window(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(new_episode("t", 1, 1, PageSpec.b4_comic()), project)
    win = MainWindow(project)
    win.resize(1280, 800)
    win.show()
    for _ in range(3):
        qapp.processEvents()
    yield win
    win.commit_now()
    win.close()


def test_the_panel_searches_and_uses_brushes_and_3d(window, qapp):
    panel = window.materials
    panel.search.setText("ハート")
    names = [panel.list.item(i).text() for i in range(panel.list.count())]
    assert "ハート" in names and len(names) < 5
    panel.search.setText("")
    panel.select_material("brush-雪ブラシ")
    panel.use()
    assert any(k.startswith("my_") for k in window.episode.brush_custom) and window.canvas.tool == "pen"
    window.use_material(next(m for m in __import__("genko.materials", fromlist=["x"]).all_materials() if m["id"] == "3d-階段"))
    window._stamp_at(100, 120)
    assert any(p["kind"] == "stairs" for p in window.episode.pages[0].prims)
