from __future__ import annotations

from pathlib import Path

from genko.export import export_print
from genko.models import Episode


def export_pack(episode: Episode, dest: Path, preset: str = "shueisha", dpi: int | None = None) -> list[Path]:
    """Submission pack at the spec resolution (B4 comic: 600 dpi): 1-bit TIFF, PNG, list.csv, README."""
    dest = Path(dest)
    dest.mkdir(parents=True, exist_ok=True)
    dpi = int(dpi or episode.spec.dpi or 600)
    tiffs = export_print(episode, dest, fmt="tiff", dpi=dpi, crop_marks=True)
    pngs = export_print(episode, dest, fmt="png", dpi=dpi, crop_marks=True)
    csv_path = dest / "list.csv"
    lines = ["page,numero,dpi,expression,spread_with,width_mm,height_mm,bleed_mm,preset"]
    for page in episode.pages:
        lines.append(
            ",".join(
                [
                    str(page.index),
                    str(page.index if page.numero else ""),
                    str(dpi),
                    page.spec.expression,
                    str(page.spread_with or ""),
                    str(page.spec.width_mm),
                    str(page.spec.height_mm),
                    str(page.spec.bleed_mm),
                    preset,
                ]
            )
        )
    csv_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    readme = dest / "README.txt"
    readme.write_text(
        f"preset={preset}\nbleed_mm={episode.spec.bleed_mm}\ninner_margin_mm={episode.spec.inner_margin_mm}\n"
        "Do not print a publisher logo.\n",
        encoding="utf-8",
    )
    return [*tiffs, *pngs, csv_path, readme]
