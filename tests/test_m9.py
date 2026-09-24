"""M9: posable mannequins, colour pages, screen profiles (webtoon / SNS), other MCP clients."""

import math
import os
import subprocess
import sys
from dataclasses import replace
from pathlib import Path

import pytest
from PIL import Image

sys.path.insert(0, str(Path(__file__).parent))

from genko import mannequin, profiles  # noqa: E402
from genko.__main__ import main  # noqa: E402
from genko.io import load_episode, save_episode  # noqa: E402
from genko.models import PageSpec, new_episode  # noqa: E402
from genko.ops import ApplyError, apply_ops  # noqa: E402
from genko.render import render_page  # noqa: E402
from genko.studio import genreq  # noqa: E402
from genko.studio.service import HumanService  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


# --- mannequin ------------------------------------------------------------------------------------


def _prim(**op) -> dict:
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_mannequin", "page": 1, "pos": [100, 160, 0], "id": "m", **op}])
    return ep.pages[0].prims[-1]


def test_standing_mannequin_is_eight_heads_tall_and_upright():
    bone = mannequin.skeleton(_prim(height_mm=80))
    x, y, w, h = bone["bbox"]
    assert 70 < h < 90  # head top to toes, about the requested height
    (hx, hy), r = bone["head"]
    assert r == pytest.approx(80 / 16) and hy < 160 < y + h  # head above the pelvis, feet below
    assert abs(hx - 100) < 1 and bone["facing"] == 1
    # the figure's left side is on the viewer's right when it faces the viewer
    left = [b for a, b, part in bone["segments"] if part == "left"]
    right = [b for a, b, part in bone["segments"] if part == "right"]
    assert sum(p[0] for p in left) / len(left) > 100 > sum(p[0] for p in right) / len(right)


@pytest.mark.parametrize("preset", sorted(mannequin.PRESETS))
def test_every_preset_gives_a_sane_figure(preset: str):
    prim = _prim(preset=preset, height_mm=60)
    assert prim["preset"] == preset and set(prim["joints"]) == set(mannequin.JOINTS)
    bone = mannequin.skeleton(prim)
    _, _, w, h = bone["bbox"]
    assert 0 < w < 80 and 20 < h < 80
    lengths = [math.dist(a, b) for a, b, _ in bone["segments"]]
    assert all(v < 60 * 0.4 for v in lengths)  # no limb longer than the figure could be


def test_side_view_presets_do_not_cross_the_body():
    bone = mannequin.skeleton(_prim(preset="walk"))
    x, y, w, h = bone["bbox"]
    assert w < h * 0.6  # seen from the side: narrow, not spread-eagled
    assert mannequin.skeleton(_prim(preset="look_back"))["facing"] == -1


def test_pose_mannequin_changes_preset_height_and_joints():
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_mannequin", "page": 1, "pos": [100, 160, 0], "id": "m"}])
    apply_ops(ep, [{"op": "pose_mannequin", "page": 1, "id": "m", "preset": "arms_up", "height_mm": 120,
                    "joints": {"head": {"yaw": 0.3}}}])
    prim = ep.pages[0].prims[-1]
    assert prim["size"][1] == 120 and prim["preset"] == "arms_up" and prim["joints"]["head"]["yaw"] == 0.3
    hands = [b for a, b, part in mannequin.skeleton(prim)["segments"] if part != "body"]
    assert min(p[1] for p in hands) < mannequin.skeleton(prim)["head"][0][1]  # hands above the head
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "pose_mannequin", "page": 1, "id": "m", "preset": "dance"}])
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "add_mannequin", "page": 1, "preset": "dance"}])


def test_mannequin_in_a_panel_becomes_the_pose_guide(tmp_path: Path):
    import test_m5 as m5

    agent, project, human = m5._project(tmp_path)
    episode = load_episode(project)
    frame = episode.pages[0].leaf_frames()[0]
    before = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data
    r = frame.rect
    apply_ops(episode, [{"op": "add_mannequin", "page": 1, "pos": [r.x + r.width / 2, r.y + r.height * 0.6, 0],
                         "height_mm": r.height * 0.6, "preset": "point", "id": "pose1"}], agent="human:leaf")
    save_episode(episode, project)
    after = agent.generation_request("demo.genko", page=1, frame_id=frame.id).data
    assert after["request"]["files"]["pose"] == "guides/pose.png" and after["request"]["color"] is False
    assert any("マネキン" in note for note in after["request"]["notes_for_agent"])
    assert not any("マネキン" in note for note in before["request"]["notes_for_agent"])
    guide = Image.open(Path(after["dir"]) / "guides" / "pose.png") if after.get("dir") else None
    if guide is not None:
        assert guide.convert("L").getextrema()[0] == 0
    # a mannequin elsewhere on the page is not this panel's pose
    other = load_episode(project)
    assert mannequin.in_rect(other.pages[0].prims[-1], (r.x, r.y, r.width, r.height))
    assert not mannequin.in_rect(other.pages[0].prims[-1], (r.x + r.width + 50, r.y, 10, 10))


# --- colour ---------------------------------------------------------------------------------------


def test_colour_pages_get_the_colour_vocabulary():
    mono, color = genreq.vocab("mono"), genreq.vocab("color")
    assert "色" in mono["avoid"] and "色" not in color["avoid"]
    assert color["style"] == color["style_color"] and "colour" in color["style"]["en"].lower()
    assert "monochrome" in mono["style"]["tags"]
    ep = new_episode("t", 1, 1, PageSpec.a4_mono())
    assert "色" not in genreq._avoid(ep, [], None, "color") and "色" in genreq._avoid(ep, [], None)


def test_finish_off_keeps_grey_and_colour_pages_are_never_screened(tmp_path: Path):
    import test_m6 as m6

    agent, project, human, frame = m6._project_with_art(tmp_path)
    episode = load_episode(project)
    page = episode.pages[0]
    rect = frame.rect

    def sky(image):
        box = (rect.x + 3, rect.y + 3, rect.x + rect.width / 3, rect.y + rect.height / 3)
        px = [round(v * 150 / 25.4) for v in box]
        return image.convert("L").crop(px).tobytes()

    assert set(sky(render_page(page, 150, mode="print", episode=episode))) <= {0, 255}
    assert len(set(sky(render_page(page, 150, mode="print", episode=episode, finish=False)))) > 2
    page.spec = replace(page.spec, expression="color")
    assert len(set(sky(render_page(page, 150, mode="print", episode=episode)))) > 2


# --- screen profiles ------------------------------------------------------------------------------


def _book(pages: int = 3) -> tuple:
    ep = new_episode("夏", 1, pages, PageSpec.a4_mono())
    for page in ep.pages:
        apply_ops(ep, [{"op": "add_mannequin", "page": page.index, "pos": [100, 160, 0]}])
    return ep


def test_webtoon_slices_the_strip(tmp_path: Path):
    ep = _book(3)
    files = profiles.export_webtoon(ep, tmp_path / "w", width_px=400, max_height=600)
    per_page = round(400 * (297 - 6) / (210 - 6))
    total = 3 * per_page
    assert len(files) == math.ceil(total / 600)
    info = [profiles.probe(f) for f in files]
    assert all(i["size"][0] == 400 and i["size"][1] <= 600 and i["format"] == "PNG" for i in info)
    assert sum(i["size"][1] for i in info) == total
    assert [f.name for f in files][:2] == ["001.png", "002.png"]
    assert all(i["icc"] for i in info) == (profiles._srgb() is not None)
    jpegs = profiles.export_webtoon(ep, tmp_path / "j", width_px=400, max_height=5000, gap_px=20, fmt="jpeg")
    assert len(jpegs) == 1 and profiles.probe(jpegs[0])["size"] == (400, total + 40)
    assert profiles.probe(jpegs[0])["format"] == "JPEG"


def test_sns_long_edge_and_spreads(tmp_path: Path):
    ep = _book(3)
    apply_ops(ep, [{"op": "set_spread", "page": 2, "with": 3}])
    files = profiles.export_sns(ep, tmp_path / "s", long_edge=512, spreads=True)
    assert len(files) == 4 and all(f.suffix == ".jpg" for f in files)
    sizes = [profiles.probe(f)["size"] for f in files]
    assert all(max(s) == 512 for s in sizes)
    assert sizes[0][1] > sizes[0][0] and sizes[-1][0] > sizes[-1][1]  # pages are tall, the spread is wide
    assert "spread_002-003" in files[-1].name
    # no dots: the screen image keeps greys (the mannequin guide is not printed; the page is white)
    with Image.open(files[0]) as img:
        assert img.mode == "RGB"


def test_trimmed_page_has_no_bleed():
    ep = _book(1)
    image = profiles.trimmed(ep.pages[0], ep, 72)
    full = render_page(ep.pages[0], 72, mode="print", episode=ep)
    bleed = round(3 / 25.4 * 72)
    assert image.size == (full.width - 2 * bleed, full.height - 2 * bleed)
    assert abs(image.width - 204 / 25.4 * 72) <= 1.5 and abs(image.height - 291 / 25.4 * 72) <= 1.5


def test_cli_and_human_export_write_screen_profiles(tmp_path: Path, capsys):
    ep = _book(2)
    project = tmp_path / "b.genko"
    save_episode(ep, project)
    assert main(["export", str(project), str(tmp_path / "cli"), "--format", "webtoon", "--width", "320", "--jpeg"]) == 0
    written = sorted((tmp_path / "cli").iterdir())
    assert written and all(f.suffix == ".jpg" and profiles.probe(f)["size"][0] == 320 for f in written)
    capsys.readouterr()
    human = HumanService(project, "human:leaf")
    assert not human.export("webtoon", tmp_path / "early")["ok"]  # preflight still guards screen formats
    ep = load_episode(project)
    for page in ep.pages:
        page.stage = "finish"
    save_episode(ep, project)
    web = human.export("webtoon", tmp_path / "web")
    assert web["ok"] and all(Path(f).is_file() for f in web["files"])
    save_episode(ep, tmp_path / "c.genko")  # the export approval makes b.genko a gated book; use a fresh copy
    sns = HumanService(tmp_path / "c.genko", "human:leaf").export("sns", tmp_path / "sns")
    assert sns["ok"] and len(sns["files"]) == 2, sns
    assert not human.export("gif", tmp_path / "x")["ok"]


# --- other MCP clients ----------------------------------------------------------------------------


def test_claude_code_skill_copy_matches_the_packaged_skill():
    packaged = (ROOT / "src/genko/studio/guide/SKILL.md").read_text(encoding="utf-8")
    copy = (ROOT / "integrations/claude-code/skills/genko-manga/SKILL.md").read_text(encoding="utf-8")
    assert copy == packaged.replace("mcp_genko_", "mcp__genko__")
    import json

    config = json.loads((ROOT / "integrations/claude-code/mcp.example.json").read_text(encoding="utf-8"))
    server = config["mcpServers"]["genko"]
    assert "mcp" in server["args"] and any(a.startswith("ai:") for a in server["args"])


def test_generic_mcp_client_example_runs(tmp_path: Path):
    pytest.importorskip("mcp")
    env = {**os.environ, "PYTHONPATH": str(ROOT / "src") + os.pathsep + os.environ.get("PYTHONPATH", "")}
    done = subprocess.run([sys.executable, str(ROOT / "integrations/generic/mcp_client_example.py"), "--root", str(tmp_path / "root")],
                          capture_output=True, text=True, timeout=180, env=env)
    assert done.returncode == 0, done.stderr[-2000:]
    import json

    report = json.loads(done.stdout.strip().splitlines()[-1])
    assert report["create"] and report["bible"] and report["script"] and report["name"]
    assert report["next"] and report["skill_chars"] > 1000
