"""作品の結合: pages of another book taken into this one. The other book's files (pictures, placed art) are
content-addressed, so copying them over never overwrites anything; the pages themselves go in by the op
import_pages."""

from __future__ import annotations

import shutil
from pathlib import Path


def copy_assets(src: Path, dest: Path) -> int:
    """Every asset file of the book at `src` into the book at `dest` (those it already has are left): how many."""
    src_assets, dest_assets = Path(src) / "assets", Path(dest) / "assets"
    if not src_assets.is_dir():
        return 0
    copied = 0
    for path in src_assets.rglob("*"):
        if not path.is_file() or path.name.endswith(".tmp"):
            continue
        target = dest_assets / path.relative_to(src_assets)
        if not target.exists():
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            copied += 1
    return copied


def import_op(src: Path, pages: list[int] | None = None, after: int | None = None) -> dict:
    op = {"op": "import_pages", "from": str(Path(src))}
    if pages:
        op["pages"] = [int(p) for p in pages]
    if after is not None:
        op["after"] = int(after)
    return op
