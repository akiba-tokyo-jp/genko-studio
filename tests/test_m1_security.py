"""M1: HTTP hardening, locks, actors, version gate, undo cost."""

import json
import os
import subprocess
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from threading import Thread

import pytest

from genko.io import load_episode, save_episode
from genko.lock import ProjectLock
from genko.migrate import UnsupportedProjectVersion
from genko.models import PageSpec, new_episode
from genko.ops import ApplyError, apply_ops
from genko.server import HeadlessServer, handle_request


@pytest.fixture()
def server(tmp_path: Path):
    srv = HeadlessServer(host="127.0.0.1", port=0, root=tmp_path, tokens={"h": "human:test", "a": "ai:bot"})
    thread = Thread(target=srv.serve_forever, daemon=True)
    thread.start()
    save_episode(new_episode("t", 1, 2, PageSpec.a4_mono()), tmp_path / "p.genko")
    yield srv
    srv.shutdown()


def _call(srv, path, body=None, headers=None, method=None):
    data = json.dumps(body).encode() if body is not None else None
    hdrs = {"Content-Type": "application/json", **(headers or {})}
    req = urllib.request.Request(f"http://127.0.0.1:{srv.port}{path}", data=data, headers=hdrs, method=method)
    try:
        with urllib.request.urlopen(req, timeout=5) as resp:
            return resp.status, dict(resp.headers), resp.read()
    except urllib.error.HTTPError as exc:
        return exc.code, dict(exc.headers), exc.read()


def test_http_requires_token_and_health_is_public(server):
    assert _call(server, "/health")[0] == 200
    assert _call(server, "/v1/inspect?path=p.genko")[0] == 401
    assert _call(server, "/v1/inspect?path=p.genko", headers={"Authorization": "Bearer wrong"})[0] == 401
    assert _call(server, "/v1/inspect?path=p.genko", headers={"Authorization": "Bearer h"})[0] == 200


def test_http_rejects_text_plain_foreign_origin_and_host(server):
    auth = {"Authorization": "Bearer h"}
    body = {"path": "p.genko", "ops": []}
    assert _call(server, "/v1/apply", body, {**auth, "Content-Type": "text/plain"})[0] == 415
    assert _call(server, "/v1/apply", body, {**auth, "Origin": "https://evil.example"})[0] == 403
    assert _call(server, "/v1/inspect?path=p.genko", headers={**auth, "Host": "evil.example"})[0] == 421
    status, headers, _ = _call(server, "/v1/inspect?path=p.genko", headers=auth)
    assert status == 200 and "Access-Control-Allow-Origin" not in headers
    assert _call(server, "/v1/apply", headers={"Origin": "https://evil.example"}, method="OPTIONS")[0] == 403


def test_http_paths_stay_inside_root(server, tmp_path: Path):
    auth = {"Authorization": "Bearer h"}
    outside = tmp_path.parent / "outside.genko"
    assert _call(server, "/v1/new", {"dest": str(outside), "pages": 1}, auth)[0] == 403
    assert _call(server, "/v1/inspect?path=../x", headers=auth)[0] == 403
    if os.name != "nt":  # creating symlinks needs extra rights on Windows
        link = tmp_path / "link"
        link.symlink_to(tmp_path.parent, target_is_directory=True)
        assert _call(server, "/v1/new", {"dest": "link/escape.genko", "pages": 1}, auth)[0] == 403
    ops = [{"op": "put_raster", "page": 1, "layer": "bg", "path": "/etc/hosts"}]
    assert _call(server, "/v1/apply", {"path": "p.genko", "ops": ops}, auth)[0] == 403


def test_http_actor_comes_from_the_token(server):
    ai = {"Authorization": "Bearer a"}
    status, _, body = _call(server, "/v1/apply", {"path": "p.genko", "ops": [{"op": "name_ok", "page": 1}]}, ai)
    assert status == 400 and b"cannot approve" in body
    assert _call(server, "/v1/apply", {"path": "p.genko", "ops": [{"op": "name_ok", "page": 1}]}, {"Authorization": "Bearer h"})[0] == 200


def test_handle_request_without_ctx_serves_no_files(tmp_path: Path):
    status, _ = handle_request("GET", f"/v1/inspect?path={tmp_path}", b"")
    assert status == 403
    assert handle_request("GET", "/openapi.json", b"")[0] == 200


def test_lock_is_exclusive_across_processes(tmp_path: Path):
    project = tmp_path / "p.genko"
    code = (
        "import sys, time; sys.path.insert(0, sys.argv[2]);"
        "from genko.lock import ProjectLock; from genko.ops import ApplyError;"
        "l = ProjectLock(__import__('pathlib').Path(sys.argv[1]), 'ai:x')\n"
        "try:\n l.acquire(); print('got', flush=True); time.sleep(1.5); l.release()\n"
        "except ApplyError: print('busy', flush=True)"
    )
    src = str(Path(__file__).resolve().parents[1] / "src")
    procs = [subprocess.Popen([sys.executable, "-c", code, str(project), src], stdout=subprocess.PIPE, text=True) for _ in range(6)]
    results = [p.communicate(timeout=30)[0].strip() for p in procs]
    assert results.count("got") == 1 and results.count("busy") == 5


def test_release_does_not_remove_a_lock_it_does_not_hold(tmp_path: Path):
    project = tmp_path / "p.genko"
    first = ProjectLock(project, "ai:a")
    first.acquire()
    second = ProjectLock(project, "ai:b")
    with pytest.raises(ApplyError):
        second.acquire()
    second.release()  # never acquired: must not touch the holder's lock
    with pytest.raises(ApplyError):
        ProjectLock(project, "ai:c").acquire()
    first.release()
    ProjectLock(project, "ai:c").acquire()


def test_a_recent_lock_written_by_an_older_build_is_respected(tmp_path: Path):
    project = tmp_path / "p.genko"
    project.mkdir()
    (project / "project.lock").write_text(json.dumps({"agent": "old", "pid": 1, "acquired_at": time.time()}))
    with pytest.raises(ApplyError):
        ProjectLock(project).acquire()
    (project / "project.lock").write_text(json.dumps({"agent": "old", "pid": 1, "acquired_at": time.time() - 3600}))
    lock = ProjectLock(project)
    lock.acquire()
    lock.release()
    assert json.loads((project / "project.lock").read_text())["released"] is True


def test_ai_cannot_approve_or_take_a_human_lock():
    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    with pytest.raises(ApplyError):
        apply_ops(ep, [{"op": "name_ok", "page": 1}], agent="ai:bot")
    apply_ops(ep, [{"op": "lock_page", "page": 1}], agent="human:leaf")
    assert ep.page_locks[ep.pages[0].id] == "human:leaf"
    for op in ({"op": "unlock_page", "page": 1}, {"op": "lock_page", "page": 1}, {"op": "lock_page", "page": 2, "agent": "human:leaf"}):
        with pytest.raises(ApplyError):
            apply_ops(ep, [op], agent="ai:bot")
    apply_ops(ep, [{"op": "lock_page", "page": 2}], agent="ai:bot")
    apply_ops(ep, [{"op": "lock_page", "page": 2}], agent="human:leaf")  # a person may take over an AI lock
    assert ep.page_locks[ep.pages[1].id] == "human:leaf"


def test_line_ops_respect_the_page_lock():
    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "x"}], agent="human:leaf")
    line_id = ep.story[0].id
    apply_ops(ep, [{"op": "lock_page", "page": 1}], agent="human:leaf")
    for op in ({"op": "edit_line", "id": line_id, "text": "y"}, {"op": "move_line", "id": line_id, "x_mm": 5}, {"op": "delete_line", "id": line_id}):
        with pytest.raises(ApplyError):
            apply_ops(ep, [op], agent="ai:bot")


def test_cli_apply_names_the_actor(tmp_path: Path, capsys):
    from genko.__main__ import main

    project = tmp_path / "p.genko"
    save_episode(new_episode("t", 1, 1, PageSpec.a4_mono()), project)
    ops = tmp_path / "ops.json"
    ops.write_text(json.dumps([{"op": "name_ok", "page": 1}]))
    assert main(["apply", str(project), str(ops), "--agent", "ai:bot"]) == 1
    (project / "studio" / "drafts").mkdir(parents=True)
    assert main(["apply", str(project), str(ops)]) == 1  # studio project + no actor = legacy:unknown
    assert main(["apply", str(project), str(ops), "--agent", "human:leaf"]) == 0
    capsys.readouterr()


def test_newer_version_refused_and_unknown_keys_kept(tmp_path: Path):
    project = tmp_path / "p.genko"
    save_episode(new_episode("t", 1, 1, PageSpec.a4_mono()), project)
    data = json.loads((project / "project.json").read_text(encoding="utf-8"))
    data["future_feature"] = {"a": 1}
    data["pages"][0]["page_future"] = [1, 2]
    (project / "project.json").write_text(json.dumps(data), encoding="utf-8")
    save_episode(load_episode(project), project)
    again = json.loads((project / "project.json").read_text(encoding="utf-8"))
    assert again["future_feature"] == {"a": 1} and again["pages"][0]["page_future"] == [1, 2]
    again["version"] = 4
    (project / "project.json").write_text(json.dumps(again), encoding="utf-8")
    with pytest.raises(UnsupportedProjectVersion):
        load_episode(project)


def test_apply_cost_does_not_grow_with_undo_depth():
    ep = new_episode("t", 1, 16, PageSpec.b4_comic())
    for page in range(1, 17):
        apply_ops(ep, [{"op": "add_stroke", "page": page, "layer": "name", "points": [[10 + i, 10 + j] for j in range(20)]} for i in range(30)])

    def cost() -> float:
        best = 1e9
        for _ in range(3):
            start = time.perf_counter()
            apply_ops(ep, [{"op": "set_note", "page": 1, "note": "x"}])
            best = min(best, time.perf_counter() - start)
        return best

    ep.undo_stack.clear()
    shallow = cost()
    while len(ep.undo_stack) < 50:
        apply_ops(ep, [{"op": "set_note", "page": 1, "note": "y"}])
    deep = cost()
    assert deep < max(0.1, shallow * 1.5)
    assert len(ep.undo_stack) == 50


def test_undo_still_restores_the_previous_state():
    ep = new_episode("t", 1, 2, PageSpec.a4_mono())
    apply_ops(ep, [{"op": "add_line", "page": 1, "text": "a"}])
    apply_ops(ep, [{"op": "edit_line", "id": ep.story[0].id, "text": "b"}])
    apply_ops(ep, [{"op": "split_frame", "page": 1, "axis": "horizontal"}])
    apply_ops(ep, [{"op": "undo"}])
    assert len(ep.pages[0].leaf_frames()) == 1 and ep.story[0].text == "b"
    apply_ops(ep, [{"op": "undo"}])
    assert ep.story[0].text == "a"


def test_cli_json_is_utf8_even_on_a_cp932_console(tmp_path: Path):
    project = tmp_path / "p.genko"
    save_episode(new_episode("日本語", 1, 1, PageSpec.a4_mono()), project)
    env = {**os.environ, "PYTHONIOENCODING": "cp932", "PYTHONPATH": str(Path(__file__).resolve().parents[1] / "src")}
    out = subprocess.run([sys.executable, "-m", "genko", "inspect", str(project)], capture_output=True, env=env)
    assert json.loads(out.stdout.decode("utf-8"))["title"] == "日本語"
    out = subprocess.run([sys.executable, "-m", "genko", "--ascii", "inspect", str(project)], capture_output=True, env=env)
    assert b"\\u65e5" in out.stdout


def test_bundled_font_ships_inside_the_package():
    from genko import render

    assert render._DELA.is_file()
    assert Path(render.__file__).resolve().parent in render._DELA.parents


def test_every_handled_op_is_in_the_catalog_and_docs():
    import re

    from genko.ops import OPS_SCHEMA

    src = (Path(__file__).resolve().parents[1] / "src" / "genko" / "ops.py").read_text(encoding="utf-8")
    handled = set(re.findall(r'if name == "(\w+)"', src))
    for group in re.findall(r"if name in \(([^)]*)\)", src):
        handled |= set(re.findall(r'"(\w+)"', group))
    listed = {item["op"] for item in OPS_SCHEMA}
    assert handled <= listed, sorted(handled - listed)
    docs = json.loads((Path(__file__).resolve().parents[1] / "docs" / "ops.schema.json").read_text(encoding="utf-8"))
    assert docs["ops"] == OPS_SCHEMA
