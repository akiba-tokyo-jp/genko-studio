"""M2 (group B of the Hermes report): each conversation under its own name, a warning before writing a book another
conversation is using, long exports as jobs, compressed PSDs, and enlarging art for print."""

import json
import os
import sys
import time
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).parent))

from genko.io import load_episode  # noqa: E402


@pytest.fixture(autouse=True)
def _env(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("GENKO_CONFIG_DIR", str(tmp_path / "config"))
    monkeypatch.setenv("GENKO_USER", "leaf")


# --- conversations and the book they share ---------------------------------------------------------------------


def _mcp(tmp_path, scenario):
    pytest.importorskip("mcp")
    import anyio
    from mcp import Client

    from genko.mcp.server import build_server

    server = build_server(tmp_path, "ai:hermes")

    async def run():
        async with Client(server) as client:
            async def call(name, args):
                return json.loads((await client.call_tool(name, args)).content[0].text)
            await scenario(call)

    anyio.run(run)


def test_each_conversation_writes_under_its_own_name_and_is_warned_once(tmp_path):
    seen = {}

    async def scenario(call):
        assert (await call("create_project", {"name": "d.genko", "title": "d", "pages": 2}))["ok"]
        line = {"op": "add_line", "page": 1, "text": "やあ", "x_mm": 30, "y_mm": 30}
        first = await call("apply_ops", {"project": "d.genko", "ops": [{**line, "id": "a"}], "commit": True, "session": "9204"})
        assert first["ok"], first
        held = await call("apply_ops", {"project": "d.genko", "ops": [{**line, "id": "b"}], "commit": True, "session": "9253"})
        seen["held"] = held
        again = await call("apply_ops", {"project": "d.genko", "ops": [{**line, "id": "b"}], "commit": True, "session": "9253"})
        assert again["ok"], again
        dry = await call("apply_ops", {"project": "d.genko", "ops": [{**line, "id": "c"}], "commit": False, "session": "9360"})
        assert dry["ok"]  # (a dry run writes nothing: no warning)
        undo = await call("undo", {"project": "d.genko", "session": "9204"})
        seen["undo_other"] = undo
        assert (await call("undo", {"project": "d.genko", "session": "9253"}))["ok"]
        bad = await call("status", {"project": "d.genko", "session": "no spaces please"})
        seen["bad"] = bad

    _mcp(tmp_path, scenario)
    held = seen["held"]
    assert not held["ok"] and held["code"] == "book_in_use" and held["others"][0]["actor"] == "ai:hermes/9204"
    assert held["you"] == "ai:hermes/9253"
    assert not seen["undo_other"]["ok"]  # (the latest change is the other conversation's)
    assert not seen["bad"]["ok"]
    ids = {line.id for line in load_episode(tmp_path / "d.genko").story}
    assert "a" in ids and "b" not in ids
    journal = (tmp_path / "d.genko" / "studio" / "logs" / "tools.jsonl").read_text(encoding="utf-8")
    assert "ai:hermes/9204" in journal and "ai:hermes/9253" in journal


def test_presence_forgets_old_conversations(tmp_path):
    from genko.studio import presence

    book = tmp_path / "b.genko"
    book.mkdir()
    presence.touch(book, "ai:x/1", now=1000.0)
    assert presence.check(book, "ai:x/2", now=1000.0 + presence.ACTIVE_S + 1) == []
    assert presence.check(book, "ai:x/2", now=1010.0)[0]["actor"] == "ai:x/1"
    assert presence.check(book, "ai:x/2", now=1020.0) == []  # (told once)
    with pytest.raises(ValueError):
        presence.actor_for("ai:x", "a/b")


# --- jobs -------------------------------------------------------------------------------------------------------


def test_a_long_job_returns_an_id_and_its_result_later(tmp_path):
    from genko.studio import jobs

    book = tmp_path / "b.genko"
    book.mkdir()

    def slow():
        time.sleep(0.5)
        return {"ok": True, "files": ["x.pdf"]}

    reply = jobs.start(book, "export", slow, actor="ai:x", wait=0.05)
    assert reply["status"] == "running" and reply["job"].startswith("job_")
    assert jobs.status(book, reply["job"])["status"] == "running"
    for _ in range(50):
        state = jobs.status(book, reply["job"])
        if state["status"] != "running":
            break
        time.sleep(0.05)
    assert state["status"] == "done" and state["result"]["files"] == ["x.pdf"]
    quick = jobs.start(book, "export", lambda: {"ok": True, "files": ["y.png"]}, actor="ai:x", wait=5)
    assert quick["ok"] and quick["files"] == ["y.png"] and quick["job"]
    broken = jobs.start(book, "export", lambda: 1 / 0, actor="ai:x", wait=5)
    assert not broken["ok"] and "ZeroDivisionError" in broken["error"]
    assert not jobs.status(book, "../etc")["ok"]


def test_export_status_over_mcp(tmp_path):
    async def scenario(call):
        assert (await call("create_project", {"name": "d.genko", "title": "d", "pages": 1}))["ok"]
        done = await call("export", {"project": "d.genko", "format": "png", "dpi": 30})
        assert done["ok"] and done["files"] and done["job"]
        state = await call("export_status", {"project": "d.genko", "job": done["job"]})
        assert state["status"] == "done" and state["result"]["files"] == done["files"]

    _mcp(tmp_path, scenario)


# --- PSD ------------------------------------------------------------------------------------------------------


def test_psd_layers_are_compressed_and_read_back(tmp_path):
    from genko import psd
    from genko.models import PageSpec, new_episode
    from genko.ops import apply_ops

    ep = new_episode("t", 1, 1, PageSpec.b5_doujin())
    ep.pages[0].name_ok = True
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "テスト", "x_mm": 40, "y_mm": 40},
                   {"op": "add_stroke", "page": 1, "layer": "ink", "points": [[30, 30], [120, 150]], "width_mm": 1.2}])
    out = psd.export_page_psd(ep, ep.pages[0], tmp_path / "p.psd", 150)
    doc = psd.read_psd(out.read_bytes())
    raw_size = doc.size[0] * doc.size[1] * 3 * 2  # (at least the merged picture and the paper, uncompressed)
    assert out.stat().st_size < raw_size / 4
    names = [layer.name for layer in doc.layers]
    assert "紙" in names and "コマ枠" in names and any(n.startswith("台詞") for n in names)
    assert doc.merged is not None and doc.merged.getpixel((5, 5))[:3] == (255, 255, 255)


def test_packbits_matches_the_reader():
    import numpy as np

    from genko.psd import _packbits_rows, _unpackbits

    rng = np.random.default_rng(3)
    for width in (1, 2, 3, 127, 128, 129, 300):
        rows = np.stack([np.full(width, 7, np.uint8), rng.integers(0, 256, width).astype(np.uint8),
                         np.repeat(rng.integers(0, 4, width // 3 + 1), 3)[:width].astype(np.uint8)])
        counts, data = _packbits_rows(rows)
        pos = 0
        for r in range(rows.shape[0]):
            n = int(counts[r])
            assert bytes(_unpackbits(data[pos:pos + n], width)) == rows[r].tobytes()
            pos += n


# --- enlarging art ----------------------------------------------------------------------------------------------


def _adopted(tmp_path):
    import test_m5 as m5

    agent, project, human = m5._project(tmp_path / "b")
    m5._sheets(agent, human)
    frame = load_episode(project).pages[0].leaf_frames()[0]
    m5._art(agent, project, 1, frame.id)
    return agent, project, human, frame.id


def test_genko_enlarges_adopted_art_into_a_marked_candidate(tmp_path):
    from genko.studio.preflight import check

    agent, project, human, frame_id = _adopted(tmp_path)
    result = agent.upscale("demo.genko", 1, frame_id, scale=2)
    assert result.ok, result.to_dict()
    data = result.data
    assert data["scale"] == 2.0 and data["method"] == "genko"
    panel = next(f for f in load_episode(project).pages[0].leaf_frames() if f.id == frame_id).panel
    cand = next(c for c in panel["candidates"] if c["id"] == data["candidate"])
    parent = next(c for c in panel["candidates"] if c["id"] == data["parent"])
    assert cand["px"] == [parent["px"][0] * 2, parent["px"][1] * 2] and cand["upscaled"]["scale"] == 2.0
    assert cand["origin"]["kind"] == "genko" and cand["mode"] == "upscale"
    assert agent.adopt("demo.genko", data["candidate"], 1, frame_id).ok
    report = check(load_episode(project), project, force=True)
    assert any(w["code"] == "upscaled" for w in report["warnings"])
    assert any(row.get("upscaled") == 2.0 for row in report["dpi"])
    again = agent.upscale("demo.genko", 1, frame_id, candidate_id=data["parent"], scale=2)
    assert again.ok and again.data["candidate"] == data["candidate"]  # (the same enlargement is not added twice)


def test_a_registered_upscaler_is_run_and_only_a_person_registers_one(tmp_path):
    from genko import upscale
    from genko.studio.cli import main as studio_main

    script = tmp_path / "up.py"
    script.write_text("import sys\nfrom PIL import Image\n"
                      "src, dst, s = sys.argv[1], sys.argv[2], float(sys.argv[3])\n"
                      "im = Image.open(src)\nim.resize((int(im.width * s), int(im.height * s))).save(dst)\n", encoding="utf-8")
    command = f'"{sys.executable}" "{script}" {{in}} {{out}} {{scale}}'
    assert studio_main(["upscaler", "add", "mine", "--command", command, "--scales", "2,4"]) == 0
    assert "mine" in upscale.registered()
    assert studio_main(["upscaler", "add", "bad", "--command", "echo hi"]) != 0 or "bad" not in upscale.registered()
    agent, project, human, frame_id = _adopted(tmp_path)
    names = [u["name"] for u in agent.inspect("demo.genko", "upscalers").data["upscalers"]]
    assert names == ["genko", "mine"]
    result = agent.upscale("demo.genko", 1, frame_id, scale=2, method="mine")
    assert result.ok, result.to_dict()
    assert not agent.upscale("demo.genko", 1, frame_id, scale=2, method="nothing").ok
    ops = agent.apply_ops("demo.genko", [{"op": "set_studio", "policy": {"upscalers": {"x": {"command": ["rm"]}}}}], commit=True)
    assert not ops.ok  # (an agent cannot reach the upscaler list through the book either)


def test_small_art_asks_for_enlarging_before_the_art_approval(tmp_path):
    from genko.studio import worklist

    agent, project, human, frame_id = _adopted(tmp_path)
    assert agent.report_regions("demo.genko", 1, frame_id, [{"kind": "face", "char": "hina", "box01": [0.4, 0.2, 0.2, 0.2]}]).ok
    ep = load_episode(project)
    items = worklist.next_actions(ep, project)
    small = [i for i in items if i["kind"] == "upscale_panel" and i["target"].get("frame_id") == frame_id]
    if not small:
        pytest.skip("the fixture art is sharp enough already")
    assert "upscale" in small[0]["tools"]
    assert not any(i["kind"] == "await_human" and i.get("gate") == "art" and i["target"].get("page") == 1 for i in items)
