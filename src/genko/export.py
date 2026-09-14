from __future__ import annotations

from pathlib import Path

from genko.models import Episode, LayerRole, Page
from genko.render import EXPORT_ROLES, export_plan, render_page

__all__ = ["EXPORT_ROLES", "export_plan", "export_png_sequence", "render_page"]


def export_png_sequence(
    episode: Episode,
    dest: Path,
    working_dpi: int = 150,
    mode: str = "print",
) -> list[Path]:
    dest.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for page in episode.pages:
        image = render_page(page, working_dpi, mode=mode, episode=episode)
        path = dest / f"{episode.title}_ep{episode.episode:02d}_p{page.index:03d}.png"
        image.save(path)
        written.append(path)
    return written
