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
    options: tuple[str, ...] = ()  # dpi, area, width, max_height, long_edge, jpeg, spreads, color, icc
    official: bool = False


FORMATS: list[Format] = [
    Format("pdf", "PDF（印刷）", "1 冊の PDF。印刷所・校正用。色は自動（モノクロはグレー）・RGB・CMYK・グレー・2 階調から。仕上がりの位置（TrimBox）入り。", ("dpi", "area", "color", "icc"), True),
    Format("tiff", "TIFF（入稿）", "ページごとの 2 値 TIFF。モノクロの入稿用。", ("dpi", "area"), True),
    Format("png", "PNG", "ページごとの PNG。", ("dpi", "area"), True),
    Format("cmyk", "CMYK（カラー入稿）", "ページごとの CMYK の TIFF。印刷所のカラープロファイル（ICC）を選ぶとそれで変換して埋め込む。"
           "選ばなければ、黒い線は K 版だけ・総インキ量は 320% 以内にして変換する。", ("dpi", "area", "icc")),
    Format("layers", "レイヤーごとの PNG", "ページごとのフォルダーに、レイヤーを 1 枚ずつ透明な PNG で（下のレイヤーから番号順）。", ("dpi", "area")),
    Format("psd", "PSD（レイヤー付き）", "ページごとの PSD。CLIP STUDIO PAINT・Photoshop で仕上げを続けるとき。", ("dpi",)),
    Format("pack", "入稿セット", "TIFF・PNG・ページ一覧（CSV）・説明書きをまとめたフォルダ。", ("dpi",)),
    Format("webtoon", "縦読み（Webtoon）", "全ページを縦につなげ、決まった高さで切った画像。網点にしない。",
           ("width", "max_height", "jpeg"), True),
    Format("sns", "SNS 用画像", "1 ページ 1 枚の JPEG。見開きも 1 枚にできる。網点にしない。", ("long_edge", "jpeg", "spreads"), True),
    Format("epub", "EPUB（電子書籍）", "固定レイアウトの EPUB 3。", ("dpi",)),
    Format("kindle", "Kindle（固定レイアウト）", "Kindle 用の固定レイアウトの電子書籍（KDP にそのまま出せる EPUB）。全ページ同じ大きさの JPEG、"
           "右綴じは右から左へ。モノクロの原稿はグレーで。", ("long_edge",)),
    Format("strip", "つなげた 1 枚", "全ページを縦に並べた 1 枚の PNG（確認用）。", ("dpi",)),
]
BY_KEY = {f.key: f for f in FORMATS}


def parse_pages(text: str, count: int) -> list[int]:
    """'3-5, 8' → [3, 4, 5, 8] (page numbers, in order, each once); ValueError when it makes no sense."""
    out: list[int] = []
    for part in (text or "").replace("、", ",").replace("，", ",").replace("〜", "-").replace("～", "-").split(","):
        part = part.strip()
        if not part:
            continue
        try:
            if "-" in part:
                a, b = (int(v) for v in part.split("-", 1))
                pages = range(min(a, b), max(a, b) + 1)
            else:
                pages = range(int(part), int(part) + 1)
        except ValueError as exc:
            raise ValueError(f"ページの指定が読めません: {part}") from exc
        for n in pages:
            if not 1 <= n <= count:
                raise ValueError(f"{n} ページはありません（1〜{count}）")
            if n not in out:
                out.append(n)
    if not out:
        raise ValueError("書き出すページがありません")
    return sorted(out)


def subset(episode: Episode, pages: list[int]) -> Episode:
    """A copy of the book with only these pages (they keep their numbers; a spread missing its partner is
    written as a single page)."""
    import copy

    keep = set(pages)
    part = copy.deepcopy(episode)
    part.pages = [page for page in part.pages if page.index in keep]
    for page in part.pages:
        if page.spread_with and page.spread_with not in keep:
            page.spread_with = None
    return part


def default_dpi(episode: Episode, key: str) -> int:
    if key in ("epub", "strip"):
        return 150
    return int(episode.spec.dpi or 600)


def run(episode: Episode, project: Path | None, key: str, out: Path, *, official: bool = False,
        actor: str = "human:user", dpi: int | None = None, width: int = 800, max_height: int = 1280,
        long_edge: int = 2048, jpeg: bool = False, spreads: bool = False, area: str = "bleed",
        pages: list[int] | None = None, color: str = "auto", icc: str | None = None) -> dict:
    """{ok, files, errors?, error?}. out is a folder. pages: only these page numbers (None: all)."""
    out = Path(out)
    fmt = BY_KEY.get(key)
    if fmt is None:
        return {"ok": False, "error": f"知らない形式: {key}"}
    if pages is not None and sorted(pages) != [p.index for p in episode.pages]:
        if official:
            return {"ok": False, "error": "正式な書き出しは全ページで行います"}
        episode = subset(episode, pages)
        if not episode.pages:
            return {"ok": False, "error": "書き出すページがありません"}
    if official:
        if not fmt.official:
            return {"ok": False, "error": f"{fmt.label} は正式な書き出しに使えない"}
        if project is None:
            return {"ok": False, "error": "正式な書き出しの前に、原稿を保存する"}
        from genko.studio.service import HumanService

        result = HumanService(project, actor).export(key, out, dpi=dpi, width_px=width, max_height=max_height,
                                                     long_edge=long_edge, jpeg=jpeg, spreads=spreads, area=area,
                                                     color=color, icc=icc)
        return result
    dpi = int(dpi or default_dpi(episode, key))
    try:
        if key in ("pdf", "tiff", "png", "cmyk"):
            from genko.export import export_print

            files = export_print(episode, out, fmt=key, dpi=dpi, area=area, color="cmyk" if key == "cmyk" else color,
                                 icc=icc or None)
        elif key == "layers":
            from genko.export import export_layers

            files = export_layers(episode, out, dpi=dpi, area=area)
        elif key == "kindle":
            from genko.export import export_kindle, stem

            files = [export_kindle(episode, out / f"{stem(episode)}_kindle.epub", long_edge=long_edge)]
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
