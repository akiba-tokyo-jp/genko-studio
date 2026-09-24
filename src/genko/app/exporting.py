"""Exports people start from the app (Qt-free): the formats, their options, and running one.

Every format can be written freely. "正式な書き出し" (official) runs the checked export instead:
preflight must pass and the export approval is recorded; it exists for pdf / tiff / png /
webtoon / sns.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from genko.models import Episode


@dataclass(frozen=True)
class Format:
    key: str
    label: str
    note: str
    options: tuple[str, ...] = ()  # dpi, width, max_height, long_edge, jpeg, spreads
    official: bool = False


FORMATS: list[Format] = [
    Format("pdf", "PDF（印刷）", "1 冊の PDF。印刷所・校正用。", ("dpi",), True),
    Format("tiff", "TIFF（入稿）", "ページごとの 2 値 TIFF。モノクロの入稿用。", ("dpi",), True),
    Format("png", "PNG", "ページごとの PNG。", ("dpi",), True),
    Format("psd", "PSD（レイヤー付き）", "ページごとの PSD。CLIP STUDIO PAINT・Photoshop で仕上げを続けるとき。", ("dpi",)),
    Format("pack", "入稿セット", "TIFF・PNG・ページ一覧（CSV）・説明書きをまとめたフォルダ。", ("dpi",)),
    Format("webtoon", "縦読み（Webtoon）", "全ページを縦につなげ、決まった高さで切った画像。網点にしない。",
           ("width", "max_height", "jpeg"), True),
    Format("sns", "SNS 用画像", "1 ページ 1 枚の JPEG。見開きも 1 枚にできる。網点にしない。", ("long_edge", "jpeg", "spreads"), True),
    Format("epub", "EPUB（電子書籍）", "固定レイアウトの EPUB 3。", ("dpi",)),
    Format("strip", "つなげた 1 枚", "全ページを縦に並べた 1 枚の PNG（確認用）。", ("dpi",)),
]
BY_KEY = {f.key: f for f in FORMATS}


def default_dpi(episode: Episode, key: str) -> int:
    if key in ("epub", "strip"):
        return 150
    return int(episode.spec.dpi or 600)


def run(episode: Episode, project: Path | None, key: str, out: Path, *, official: bool = False,
        actor: str = "human:user", dpi: int | None = None, width: int = 800, max_height: int = 1280,
        long_edge: int = 2048, jpeg: bool = False, spreads: bool = False) -> dict:
    """{ok, files, errors?, error?}. out is a folder."""
    out = Path(out)
    fmt = BY_KEY.get(key)
    if fmt is None:
        return {"ok": False, "error": f"知らない形式: {key}"}
    if official:
        if not fmt.official:
            return {"ok": False, "error": f"{fmt.label} は正式な書き出しに使えない"}
        if project is None:
            return {"ok": False, "error": "正式な書き出しの前に、原稿を保存する"}
        from genko.studio.service import HumanService

        result = HumanService(project, actor).export(key, out, dpi=dpi, width_px=width, max_height=max_height,
                                                     long_edge=long_edge, jpeg=jpeg, spreads=spreads)
        return result
    dpi = int(dpi or default_dpi(episode, key))
    try:
        if key in ("pdf", "tiff", "png"):
            from genko.export import export_print

            files = export_print(episode, out, fmt=key, dpi=dpi)
        elif key == "psd":
            from genko.psd import export_psd_pages

            files = export_psd_pages(episode, out, dpi=dpi)
        elif key == "pack":
            from genko.pack import export_pack

            files = export_pack(episode, out, dpi=dpi)
        elif key == "epub":
            from genko.export import export_epub, stem

            files = [export_epub(episode, out / f"{stem(episode)}.epub", dpi=dpi)]
        elif key == "strip":
            from genko.export import export_strip, stem

            files = [export_strip(episode, out / f"{stem(episode)}_strip.png", dpi=dpi)]
        elif key == "webtoon":
            from genko import profiles

            files = profiles.export_webtoon(episode, out, width, max_height, fmt="jpeg" if jpeg else "png")
        else:
            from genko import profiles

            files = profiles.export_sns(episode, out, long_edge, fmt="jpeg" if jpeg else "png", spreads=spreads)
    except (OSError, ValueError) as exc:
        return {"ok": False, "error": str(exc)}
    return {"ok": True, "files": [str(p) for p in files]}
