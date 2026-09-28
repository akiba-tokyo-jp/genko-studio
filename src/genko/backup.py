"""バックアップ: the whole book zipped into another folder now and then (after a save), the newest `keep` kept."""

from __future__ import annotations

import time
import zipfile
from pathlib import Path


def backups(folder: Path, stem: str) -> list[Path]:
    """This book's backups in the folder, oldest first."""
    folder = Path(folder)
    if not folder.is_dir():
        return []
    return sorted(folder.glob(f"{stem}_????????-??????.zip"))


def make(project: Path, folder: Path, keep: int = 10, now: float | None = None) -> Path:
    """Zip the book into the folder (named by the time) and drop the oldest past `keep`."""
    project, folder = Path(project), Path(folder)
    if project.resolve() in (folder.resolve(), *folder.resolve().parents):
        raise ValueError("バックアップのフォルダーは、原稿のフォルダーの外にしてください")
    folder.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(now if now is not None else time.time()))
    out = folder / f"{project.stem}_{stamp}.zip"
    tmp = out.with_suffix(".zip.part")
    with zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for path in sorted(project.rglob("*")):
            if path.is_file() and not path.name.endswith((".tmp", ".lock")):
                zf.write(path, f"{project.name}/{path.relative_to(project)}")
    tmp.replace(out)
    for old in backups(folder, project.stem)[: max(0, len(backups(folder, project.stem)) - max(1, int(keep)))]:
        old.unlink(missing_ok=True)
    return out


def due(folder: Path, stem: str, minutes: float, now: float | None = None) -> bool:
    """Whether the last backup of this book is older than `minutes` (or there is none)."""
    last = backups(folder, stem)
    if not last:
        return True
    return (now if now is not None else time.time()) - last[-1].stat().st_mtime >= minutes * 60
