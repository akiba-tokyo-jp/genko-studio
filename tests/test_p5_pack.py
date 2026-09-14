from pathlib import Path

from genko.models import PageSpec, new_episode
from genko.pack import export_pack


def test_pack_writes_tiff_csv_and_readme(tmp_path: Path):
    ep = new_episode("t", 1, 2, PageSpec.b4_comic())
    files = {p.name for p in export_pack(ep, tmp_path, preset="shueisha")}
    assert any(name.endswith(".tiff") for name in files)
    assert "list.csv" in files
    assert "README.txt" in files
