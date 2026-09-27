"""The AI's feedback after the Windows run: references in order of need (an image tool may take two), props
written by their name, and the enlargement's candidate id under the name adopt uses."""

import json
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))

from genko.studio import genreq  # noqa: E402
from genko.studio.service import StudioService  # noqa: E402

FIXTURES = Path(__file__).parent / "fixtures" / "studio" / "demo4"


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))


def _load(name: str) -> dict:
    return json.loads((FIXTURES / name).read_text(encoding="utf-8"))


def test_references_come_most_needed_first():
    names = ["refs/loc_park_12345678.png", "refs/previous_panel.png", "refs/prop_cup_1234.png", "refs/style_catalog.png",
             "refs/hina_sheet.png", "refs/hina_face.png"]
    assert genreq._by_priority(names) == ["refs/hina_face.png", "refs/hina_sheet.png", "refs/style_catalog.png",
                                          "refs/previous_panel.png", "refs/prop_cup_1234.png", "refs/loc_park_12345678.png"]


def test_a_prop_written_by_its_name_is_read_as_its_id(tmp_path: Path):
    agent = StudioService(tmp_path, "ai:test")
    assert agent.create_project("demo.genko", "demo", 4).ok
    bible = _load("bible.json")
    bible["props"] = [{"id": "cup", "name": "青いマグカップ", "desc": "取っ手の欠けた青いマグ"}]
    assert agent.set_bible("demo.genko", bible, commit=True).ok
    assert agent.set_script("demo.genko", _load("script.json"), commit=True).ok
    plan = _load("p001.json")
    plan["panels"][0]["props"] = ["青いマグカップ"]
    result = agent.submit_name("demo.genko", plan)
    codes = {i.code for i in result.issues}
    assert result.ok and "prop_by_name" in codes and "unknown_prop" not in codes
