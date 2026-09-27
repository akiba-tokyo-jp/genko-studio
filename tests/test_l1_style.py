"""L1: the manga style catalog — reading a branch, keeping it in the book, the art requests that carry it, the
agent tools, the window's picker and genko:// links. The catalog is faked (no network)."""

import io
import json
import os
import sys
from pathlib import Path

import pytest
from PIL import Image

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko import stylecat  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.studio import genreq  # noqa: E402
from genko.studio.service import StudioService  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"
BASE = "https://catalog.test"
WORDS = ("モノクロの日本のマンガのコマ。灰色はすべて網点で表す。 バトル漫画の絵柄。荒々しく太い線。 "
         "仕上げの絵柄（上と違う所はこちらに従う）: 2〜3頭身のデフォルメ。")


def _jpeg() -> bytes:
    out = io.BytesIO()
    Image.new("L", (30, 40), 128).save(out, format="JPEG")
    return out.getvalue()


def _branch(sid: str, title: str, level: int, parent_path: list, children: list, version: int = 3) -> dict:
    return {"id": sid, "title": title, "summary": f"{title}の絵柄。", "level": level, "expression": "mono",
            "path": [*parent_path, {"id": sid, "title": title}],
            "prompt": {"ja": WORDS, "en": "Black-and-white manga panel. Finished style: chibi.", "tags": "manga, chibi"},
            "avoid": ["文字", "フキダシ", "効果音の描き文字", "コマ枠", "署名", "透かし", "色"],
            "samples": [{"subject": "upper_body", "url": f"{BASE}/files/{sid}/upper_body-v1.jpg"}], "reference": 0,
            "license": {"generated": True, "artist_names": False, "terms": "見本の絵はすべて生成 AI"},
            "version": version, "updated_at": "2026-09-27T11:18:12Z", "children": children}


CATALOG = {
    "shonen": _branch("shonen", "少年漫画", 1, [], [{"id": "shonen-battle", "title": "バトル", "summary": "戦い"}]),
    "shonen-battle": _branch("shonen-battle", "バトル", 2, [{"id": "shonen", "title": "少年漫画"}],
                             [{"id": "shonen-battle-chibi", "title": "低頭身のデフォルメ", "summary": "2〜3頭身"}]),
    "shonen-battle-chibi": _branch("shonen-battle-chibi", "低頭身のデフォルメ", 3,
                                   [{"id": "shonen", "title": "少年漫画"}, {"id": "shonen-battle", "title": "バトル"}], []),
}
TREE = {"version": 1, "nodes": [
    {"id": "shonen", "parent": None, "level": 1, "title": "少年漫画", "summary": "熱い", "thumbnail_url": f"{BASE}/t/shonen.jpg"},
    {"id": "shonen-battle", "parent": "shonen", "level": 2, "title": "バトル", "summary": "戦い", "thumbnail_url": None},
    {"id": "shonen-battle-chibi", "parent": "shonen-battle", "level": 3, "title": "低頭身のデフォルメ", "summary": "2〜3頭身",
     "thumbnail_url": None},
]}


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")
    monkeypatch.setenv("GENKO_STYLE_CATALOG", BASE)
    asked: list[str] = []

    def fake(url: str) -> bytes:
        asked.append(url)
        if url == f"{BASE}/v1/tree":
            return json.dumps(TREE).encode()
        if url.startswith(f"{BASE}/v1/styles/"):
            sid = url.rsplit("/", 1)[1]
            if sid not in CATALOG:
                raise stylecat.CatalogError("絵柄カタログにその絵柄がない（id を確かめる）")
            return json.dumps(CATALOG[sid]).encode()
        if url.endswith(".jpg"):
            return _jpeg()
        raise AssertionError(url)

    monkeypatch.setattr(stylecat, "_get", fake)
    return asked


def _load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def _named(root: Path, spec: str = "commercial-b4") -> StudioService:
    agent = StudioService(root, "ai:test")
    assert agent.create_project("demo.genko", "demo", 4, spec).ok
    assert agent.set_bible("demo.genko", _load("bible.json"), commit=True).ok
    assert agent.set_script("demo.genko", _load("script.json"), commit=True).ok
    assert agent.submit_name("demo.genko", _load("p001.json"), commit=True).ok
    return agent


def test_links_and_ids():
    assert stylecat.style_from_link("genko://use-style?id=shonen-battle") == "shonen-battle"
    assert stylecat.style_from_link("genko://other?id=x") is None
    with pytest.raises(stylecat.CatalogError):
        stylecat.style_from_link("genko://use-style?id=../../etc")
    assert stylecat.page_url("shonen") == f"{BASE}/n/shonen?lang=ja"


def test_the_book_keeps_the_style_and_the_art_requests_carry_it(tmp_path: Path):
    agent = _named(tmp_path)
    look = agent.use_style("demo.genko", "shonen-battle-chibi")  # (a look first: nothing is written)
    assert look.ok and not look.data["committed"] and look.data["style"]["path"] == ["少年漫画", "バトル", "低頭身のデフォルメ"]
    assert genreq.catalog(load_episode(tmp_path / "demo.genko")) is None
    done = agent.use_style("demo.genko", "shonen-battle-chibi", commit=True)
    assert done.ok and done.data["style"]["id"] == "shonen-battle-chibi" and done.data["style"]["has_sample"]
    episode = load_episode(tmp_path / "demo.genko")
    kept = genreq.catalog(episode)
    assert kept["version"] == 3 and kept["prompt"]["ja"] == WORDS and kept["sample"].startswith("sha256:")
    assert agent.status("demo.genko").data["style"]["title"] == "低頭身のデフォルメ"

    frame = episode.pages[0].leaf_frames()[0]
    pack = genreq.build(episode, tmp_path / "demo.genko", page=1, frame_id=frame.id)
    request = pack.request
    assert request["prompt"]["ja"].startswith(WORDS)
    assert "Finished style: chibi" in request["prompt"]["en"] and "chibi" in request["prompt"]["tags"]
    assert sum("透かし" in a for a in request["avoid"]) == 1 and "色" in request["avoid"]  # (no word twice)
    assert "refs/style_catalog.png" in request["files"]["references"] and "refs/style_catalog.png" in pack.files
    assert request["style"] == {"catalog_id": "shonen-battle-chibi", "version": 3, "title": "低頭身のデフォルメ"}
    sheet = genreq.build(episode, tmp_path / "demo.genko", purpose="character_sheet",
                         character_id=episode.bible.characters[0]["id"]).request
    assert sheet["prompt"]["ja"].startswith("モノクロの日本のマンガの絵。")  # (a sheet is a picture, not a panel)

    # the site changes: the book does not; the agent can see a newer version
    CATALOG["shonen-battle-chibi"]["version"] = 4
    try:
        seen = agent.style_catalog("demo.genko").data
        assert seen["style"]["version"] == 3 and seen["newer"]["version"] == 4
    finally:
        CATALOG["shonen-battle-chibi"]["version"] = 3
    back = agent.use_style("demo.genko", None, commit=True)
    assert back.ok and genreq.catalog(load_episode(tmp_path / "demo.genko")) is None


def test_browsing_the_catalog(tmp_path: Path):
    agent = StudioService(tmp_path, "ai:test")
    genres = agent.style_catalog().data["genres"]
    assert [g["id"] for g in genres] == ["shonen"]
    node = agent.style_catalog(style_id="shonen-battle").data["style"]
    assert node["children"][0]["id"] == "shonen-battle-chibi" and node["prompt_ja"] == WORDS
    missing = agent.style_catalog(style_id="nope")
    assert not missing.ok and "ない" in missing.issues[0].message


def test_a_black_and_white_style_on_a_colour_book_is_told_and_not_used(tmp_path: Path):
    agent = _named(tmp_path, "webtoon")
    done = agent.use_style("demo.genko", "shonen", commit=True)
    assert done.ok and "style_mono_on_color" in {i.code for i in done.issues}
    episode = load_episode(tmp_path / "demo.genko")
    frame = episode.pages[0].leaf_frames()[0]
    request = genreq.build(episode, tmp_path / "demo.genko", page=1, frame_id=frame.id).request
    assert not request["prompt"]["ja"].startswith(WORDS) and "style" not in request
    assert any("白黒用" in n for n in request["notes_for_agent"])


def test_once_the_pilot_fixed_the_style_only_a_person_changes_it(tmp_path: Path):
    ep = new_episode("t", 1, 1, PageSpec.b4_comic())
    ep.studio["style"] = {"locked": {"page": 1}}
    kept = stylecat.saved(CATALOG["shonen"], None)
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_style_catalog", "catalog": kept}], agent="ai:hermes")
    apply_ops(ep, [{"op": "set_style_catalog", "catalog": kept}], agent="human:leaf")
    assert ep.studio["style"]["catalog"]["id"] == "shonen" and ep.studio["style"]["locked"]
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "set_style_catalog", "catalog": {"id": "x", "prompt": {}}}], agent="human:leaf")


# --- the window -------------------------------------------------------------------------------------------------


@pytest.fixture(scope="module")
def qapp():
    QtWidgets = pytest.importorskip("PySide6.QtWidgets")
    return QtWidgets.QApplication.instance() or QtWidgets.QApplication([])


def test_the_picker_walks_the_levels(qapp, monkeypatch):
    from genko.app.style_picker import StylePicker

    monkeypatch.setattr(StylePicker, "inline", True)
    dialog = StylePicker(None)
    assert dialog.list.count() == 1 and not dialog.up.isEnabled()
    dialog.list.setCurrentRow(0)
    assert dialog.ok.isEnabled() and "網点" in dialog.words.text()
    dialog._go_down("shonen")
    dialog._go_down("shonen-battle")
    assert dialog.list.item(0).data(256) == "shonen-battle-chibi" and "3段目" in dialog.where.text()
    dialog.list.setCurrentRow(0)
    dialog._accept()
    assert dialog.chosen == "shonen-battle-chibi"
    again = StylePicker(None, stylecat.saved(CATALOG["shonen-battle-chibi"], None))  # (opens where the book's style is)
    assert again.here == "shonen-battle" and again.chosen_id() == "shonen-battle-chibi" and not again.plain.isHidden()
    dialog.close()
    again.close()


def test_the_window_takes_a_style_from_a_link(qapp, tmp_path: Path):
    from genko.app.main import MainWindow

    project = tmp_path / "b.genko"
    save_episode(new_episode("t", 1, 2, PageSpec.b4_comic()), project)
    window = MainWindow(project)
    try:
        assert window.use_style("shonen-battle", ask=False)
        kept = genreq.catalog(window.episode)
        assert kept["id"] == "shonen-battle" and kept["sample"]
        window.commit_now()
        assert genreq.catalog(load_episode(project))["id"] == "shonen-battle"
        window.open_link("genko://use-style?id=../x")  # (a bad id: a notice, nothing changes)
        assert genreq.catalog(window.episode)["id"] == "shonen-battle"
    finally:
        window.close()


def test_a_second_start_hands_its_link_to_the_open_genko(qapp):
    from genko.app import links

    listener = links.Listener()
    got: list[str] = []
    listener.received.connect(got.append)
    try:
        assert links.send("genko://use-style?id=shonen")
        for _ in range(50):
            qapp.processEvents()
            if got:
                break
        assert got == ["genko://use-style?id=shonen"]
    finally:
        listener.server.close()
        listener.deleteLater()
        qapp.processEvents()
    assert links.is_link("GENKO://x") and not links.is_link("C:/book.genko")
