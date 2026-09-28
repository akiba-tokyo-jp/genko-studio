"""What stops the final export, and watermarked proofs the agent may export.

Every refusal has a reason and a place. Low resolution is refused unless
forced; test images (origin.kind "fixture") are refused unless allowed.
Missing provenance only warns.
"""

from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from genko.assets import AssetStore
from genko.models import Episode, LayerKind, Page
from genko.studio.issues import Issue, error, warning

MIN_DPI = 350  # grey art; line art that is thresholded wants 600 (M6)
PROOF_DPI = 150


def _image_size(store: AssetStore, ref: str) -> tuple[int, int] | None:
    path = store.path(ref, ".png")
    if not path.is_file():
        return None
    with Image.open(path) as img:
        return img.size


def effective_dpi(width_px: int, width_mm: float) -> float:
    return width_px / (width_mm / 25.4) if width_mm > 0 else 0.0


def _candidate_for(page: Page, layer) -> dict | None:
    cand_id = (layer.source or {}).get("candidate")
    if not cand_id or not layer.frame_id:
        return None
    try:
        frame = page._find(layer.frame_id)
    except (KeyError, IndexError):
        return None
    return next((c for c in (frame.panel or {}).get("candidates", []) if c.get("id") == cand_id), None)


def art_layers(page: Page) -> list:
    return [layer for layer in page.layers if layer.kind == LayerKind.PLACED and layer.exportable and layer.visible]


def layer_dpi(episode: Episode, page: Page, layer, store: AssetStore) -> float | None:
    if not layer.asset or layer.placement_mm is None:
        return None
    size = _image_size(store, layer.asset)
    if size is None:
        return None
    return round(effective_dpi(size[0], layer.placement_mm.width), 1)


def check(episode: Episode, project: Path, *, allow_fixture: bool = False, force: bool = False, min_dpi: int = MIN_DPI,
          color: str | None = None) -> dict:
    store = AssetStore(project)
    errors: list[Issue] = []
    warnings: list[Issue] = []
    dpi_table: list[dict] = []
    studio_project = bool(episode.strict_gates or episode.studio)
    chars = {c.get("id"): c for c in episode.bible.characters}
    needed_sheets: set[str] = set()
    for page in episode.pages:
        where = f"/pages/{page.index}"
        if studio_project:
            if not page.name_ok:
                errors.append(error("name_not_approved", where, f"{page.index} ページのネームが承認されていない"))
            if not page.art_ok:
                errors.append(error("art_not_approved", where, f"{page.index} ページの作画が承認されていない"))
        if page.stage != "finish":
            errors.append(error("page_not_finished", where, f"{page.index} ページが仕上げ（finish）に進んでいない"))
        for frame in page.leaf_frames():
            panel = frame.panel
            if panel is None:
                continue
            for c in panel.get("characters", []):
                if isinstance(c, dict) and c.get("id") in chars:
                    needed_sheets.add(c["id"])
            if panel.get("status") not in ("adopted", "skip"):
                errors.append(error("panel_without_art", f"{where}/frames/{frame.id}",
                                    f"{page.index} ページのコマ {panel.get('slot') or frame.id} に採用した絵が無い"))
        for line in episode.story_for_page(page.index):
            if not line.x_mm and not line.y_mm:
                errors.append(error("line_unplaced", f"{where}/lines/{line.id}", f"{page.index} ページの台詞「{line.text[:12]}」に位置が無い"))
        for layer in art_layers(page):
            lwhere = f"{where}/layers/{layer.id}"
            if layer.asset and not store.path(layer.asset, ".png").is_file():
                errors.append(error("asset_missing", lwhere, f"{page.index} ページの画像 {layer.asset[:19]}… が assets/ に無い"))
                continue
            dpi = layer_dpi(episode, page, layer, store)
            if dpi is not None:
                dpi_table.append({"page": page.index, "frame_id": layer.frame_id, "dpi": dpi})
                if dpi < min_dpi:
                    issue = (warning if force else error)(
                        "low_dpi", lwhere, f"{page.index} ページのコマ {layer.frame_id} の実効解像度が {dpi:.0f} dpi（{min_dpi} 未満）",
                        "高解像度化した候補を取り込むか、--force で通す")
                    (warnings if force else errors).append(issue)
            cand = _candidate_for(page, layer)
            origin = (cand or {}).get("origin") or {}
            if (cand or {}).get("upscaled"):  # (enlarged: prints at size, but nothing more was drawn)
                up = cand["upscaled"]
                if dpi_table and dpi_table[-1].get("frame_id") == layer.frame_id:
                    dpi_table[-1]["upscaled"] = up.get("scale")
                warnings.append(warning("upscaled", lwhere, f"{page.index} ページのコマ {layer.frame_id} の絵は {up.get('scale')} 倍に拡大したもの"
                                        f"（{up.get('method')}。描き込みは元の大きさのまま）"))
            if origin.get("kind") == "fixture":
                issue = (warning if allow_fixture else error)(
                    "fixture_image", lwhere, f"{page.index} ページに試験用の画像（fixture）がある", "本番では使えない。--allow-fixture で通す")
                (warnings if allow_fixture else errors).append(issue)
            elif cand is None or (origin.get("kind") == "agent" and not (origin.get("tool_id") or origin.get("model"))):
                warnings.append(warning("provenance_missing", lwhere, f"{page.index} ページのコマ {layer.frame_id} の画像の来歴（ツール・モデル）が記録されていない"))
    mono = any(p.spec.expression != "color" for p in episode.pages)
    if color == "rgb" and mono:  # (a monochrome book written in colour: the printer sees grey fringes on every dot)
        issue = (warning if force else error)("mono_as_colour", "/color", "モノクロの原稿を RGB で書き出そうとしている",
                                              "color を auto（グレー）か bitonal（2 階調）にする")
        (warnings if force else errors).append(issue)
    if studio_project:
        for cid in sorted(needed_sheets):
            if not chars[cid].get("locked"):
                errors.append(error("sheet_not_approved", f"/characters/{cid}", f"{cid} の設定画が承認されていない"))
    return {
        "ok": not errors,
        "errors": [i.to_dict() for i in errors],
        "warnings": [i.to_dict() for i in warnings],
        "dpi": dpi_table,
        "min_dpi": min_dpi,
    }


def watermark(image: Image.Image, text: str) -> Image.Image:
    from genko.render import _DELA

    base = image.convert("RGB")
    size = max(24, base.width // 9)
    try:
        font = ImageFont.truetype(str(_DELA), size)
    except OSError:
        font = ImageFont.load_default()
    layer = Image.new("L", base.size, 0)
    draw = ImageDraw.Draw(layer)
    step_y = size * 4
    for y in range(-base.height, base.height * 2, step_y):
        draw.text((base.width * 0.1, y), text, fill=70, font=font)
    rotated = layer.rotate(math.degrees(math.atan2(base.height, base.width)) - 90 + 60, resample=Image.Resampling.BILINEAR)
    grey = Image.new("RGB", base.size, (150, 150, 150))
    base.paste(grey, (0, 0), rotated)
    return base


def export_proof(episode: Episode, project: Path, fmt: str = "pdf", dpi: int = PROOF_DPI) -> list[Path]:
    """Every page at proof resolution with a 校正 watermark, into studio/proofs/r<revision>/."""
    from genko.render import render_page

    if fmt not in ("pdf", "png"):
        raise ValueError("format は pdf か png")
    dpi = min(dpi, PROOF_DPI)
    out = Path(project) / "studio" / "proofs" / f"r{episode.revision:04d}"
    out.mkdir(parents=True, exist_ok=True)
    label = f"校正 PROOF r{episode.revision}"
    images = [watermark(render_page(page, dpi, mode="print", episode=episode), label) for page in episode.pages]
    if fmt == "pdf":
        path = out / "proof.pdf"
        images[0].save(path, format="PDF", save_all=True, append_images=images[1:], resolution=dpi)
        return [path]
    written = []
    for page, image in zip(episode.pages, images):
        path = out / f"p{page.index:03d}.png"
        image.save(path)
        written.append(path)
    return written
