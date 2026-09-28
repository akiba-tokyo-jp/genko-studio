"""genko gc and genko doctor."""

from __future__ import annotations

import json
import shutil
import time
from pathlib import Path

from genko import journal
from genko.assets import AssetStore
from genko.lock import ProjectLock

KEEP_NEW_SECONDS = 24 * 3600  # assets written outside the lock (e.g. an import in progress) survive a day


def gc(project: Path, *, dry_run: bool = True, legacy: bool = False, agent: str = "genko") -> dict:
    """Delete assets nothing refers to (current project, journal snapshots, studio drafts)."""
    project = Path(project)
    with ProjectLock(project, agent=agent):
        keep = journal.referenced_assets(project)
        store = AssetStore(project)
        now = time.time()
        removed: list[str] = []
        for path in store.all_files():
            ref = "sha256:" + path.name.split(".", 1)[0]
            if ref in keep or now - path.stat().st_mtime < KEEP_NEW_SECONDS:
                continue
            removed.append(str(path.relative_to(project)))
            if not dry_run:
                path.unlink()
        legacy_dir = project / "pages"
        legacy_removed = False
        if legacy and legacy_dir.is_dir():
            payload = json.loads((project / "project.json").read_text(encoding="utf-8"))
            if payload.get("version", 1) >= 3:
                legacy_removed = True
                if not dry_run:
                    shutil.rmtree(legacy_dir)
    return {"ok": True, "dry_run": dry_run, "removed": removed, "legacy_pages_removed": legacy_removed}


def doctor(project: Path) -> dict:
    """Report missing assets, the font, and long Windows paths."""
    from genko.render import _DELA

    project = Path(project).resolve()
    problems: list[str] = []
    payload = json.loads((project / "project.json").read_text(encoding="utf-8"))
    store = AssetStore(project)
    for page in payload.get("pages", []):
        for layer in page.get("layers", []):
            for key, suffix in (("asset", ".png"), ("strokes_blob", ".strokes.json")):
                ref = layer.get(key)
                if ref and not store.path(ref, suffix).is_file():
                    problems.append(f"page {page.get('index')} layer {layer.get('id')}: missing {store.relpath(ref, suffix)}")
    if not _DELA.is_file():
        problems.append(f"bundled font missing: {_DELA}")
    if len(str(project)) > 150:
        problems.append(f"project path is {len(str(project))} characters; Windows may refuse deep asset paths")
    return {"ok": not problems, "version": payload.get("version"), "revision": payload.get("revision"), "problems": problems}
