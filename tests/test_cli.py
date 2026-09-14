from pathlib import Path

from genko.__main__ import main


def test_cli_new_and_export(tmp_path: Path):
    dest = tmp_path / "demo.genko"
    assert main(["new", str(dest), "--title", "demo", "--pages", "3"]) == 0
    assert (dest / "project.json").is_file()
    out = tmp_path / "out"
    assert main(["export", str(dest), str(out)]) == 0
    assert len(list(out.glob("*.png"))) == 3
