import json
from pathlib import Path

from genko.__main__ import main


def test_cli_new_and_export(tmp_path: Path):
    dest = tmp_path / "demo.genko"
    assert main(["new", str(dest), "--title", "demo", "--pages", "3"]) == 0
    assert (dest / "project.json").is_file()
    out = tmp_path / "out"
    assert main(["export", str(dest), str(out)]) == 0
    assert len(list(out.glob("*.png"))) == 3


def test_cli_inspect_and_apply_are_json(tmp_path: Path, capsys):
    dest = tmp_path / "demo.genko"
    assert main(["new", str(dest), "--title", "demo", "--pages", "2", "--json"]) == 0
    created = json.loads(capsys.readouterr().out)
    assert created["ok"] is True
    assert created["snapshot"]["title"] == "demo"

    ops = tmp_path / "ops.json"
    ops.write_text(
        json.dumps(
            [
                {"op": "add_line", "page": 1, "text": "始めよう。", "speaker": "主人公"},
                {"op": "name_ok", "page": 1},
            ]
        ),
        encoding="utf-8",
    )
    assert main(["apply", str(dest), str(ops)]) == 0
    applied = json.loads(capsys.readouterr().out)
    assert applied["ok"] is True
    assert applied["snapshot"]["pages"][0]["name_ok"] is True

    assert main(["inspect", str(dest)]) == 0
    inspected = json.loads(capsys.readouterr().out)
    assert inspected["pages"][0]["story"][0]["text"] == "始めよう。"


def test_cli_schema_lists_ops(capsys):
    assert main(["schema"]) == 0
    data = json.loads(capsys.readouterr().out)
    names = {item["op"] for item in data["ops"]}
    assert "split_frame" in names
    assert "name_ok" in names
