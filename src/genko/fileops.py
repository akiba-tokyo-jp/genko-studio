"""J10 ops for files: a PSD (or PSB) read in as layers (PSD の読み込み), and the timelapse switch (タイムラプス)."""

from __future__ import annotations

import base64
import io
import struct
from pathlib import Path
from typing import Any

from PIL import Image

from genko.models import Layer, LayerKind, LayerRole, new_id
from genko.ops import ApplyError

OPS = ("import_psd", "set_timelapse")
MAX_SIDE = 12_000  # (a page layer longer than this is scaled down: the file would not be usable anyway)
FITS = ("paper", "bleed", "trim")


def _png(image: Image.Image) -> bytes:
    buf = io.BytesIO()
    image.save(buf, format="PNG")
    return buf.getvalue()


def _target(page, fit: str):
    return {"paper": None, "bleed": page.bleed_rect_mm(), "trim": page.trim_rect_mm()}[fit]


def placed(page, size: tuple[int, int], fit: str) -> tuple[tuple[int, int], tuple[int, int], tuple[int, int]]:
    """Where a picture of `size` px lands on the page: (the page layer's size px, the picture's size px on it,
    its top-left px). It keeps its shape and fills `fit` (the paper, the bleed or the finished size) as far as
    it can, centred; the layer's resolution follows the picture's."""
    width_mm, height_mm = page.spec.width_mm, page.spec.height_mm
    rect = _target(page, fit)
    x, y, w, h = (0.0, 0.0, width_mm, height_mm) if rect is None else (rect.x, rect.y, rect.width, rect.height)
    px_per_mm = min(size[0] / w, size[1] / h)  # (the picture's own resolution when it fits the area)
    scale = 1.0
    longest = max(width_mm, height_mm) * px_per_mm
    if longest > MAX_SIDE:
        scale = MAX_SIDE / longest
        px_per_mm *= scale
    page_px = (max(1, round(width_mm * px_per_mm)), max(1, round(height_mm * px_per_mm)))
    shown = (max(1, round(size[0] * scale)), max(1, round(size[1] * scale)))
    fit_w, fit_h = w * px_per_mm, h * px_per_mm
    at = (round(x * px_per_mm + (fit_w - shown[0]) / 2), round(y * px_per_mm + (fit_h - shown[1]) / 2))
    return page_px, shown, at


def import_psd(episode, op: dict) -> None:
    """Every layer of a PSD as a Genko layer on the page, from the bottom: pixels (text layers as their
    pictures), names, opacity, visibility, blend mode, clipping, folders and layer masks. Adjustment and fill
    layers carry no pixels and are left out (named in the op's report)."""
    from genko import psd
    from genko.ops import MASK_DPI, MAX_IMAGE_PIXELS, _require_page

    page = _require_page(episode, op)
    if op.get("psd"):
        try:
            data = base64.b64decode(str(op["psd"]))
        except ValueError as exc:
            raise ApplyError("psd is the file's bytes in base64") from exc
    elif op.get("path"):
        source = Path(str(op["path"])).expanduser()
        if not source.is_absolute() and episode.asset_dir is not None:  # (relative to the book's folder)
            source = Path(episode.asset_dir) / source
        try:
            data = source.read_bytes()
        except OSError as exc:
            raise ApplyError(f"the file cannot be read ({exc})") from exc
    else:
        raise ApplyError("import_psd needs path or psd (base64)")
    fit = str(op.get("fit") or "bleed")
    if fit not in FITS:
        raise ApplyError("fit must be paper, bleed or trim")
    try:
        doc = psd.read_psd(data)
    except (psd.PSDError, struct.error, ValueError, IndexError) as exc:
        raise ApplyError(f"the PSD cannot be read ({exc})") from exc
    if doc.size[0] * doc.size[1] > MAX_IMAGE_PIXELS:
        raise ApplyError(f"image too large: {doc.size[0]}x{doc.size[1]}")
    layers = doc.layers
    if not any(item.image is not None for item in layers):
        if doc.merged is None:
            raise ApplyError("the PSD has no pictures to read")
        layers = [psd.PSDLayer(name=str(op.get("name") or "PSD"), image=doc.merged)]
    page_px, shown, at = placed(page, doc.size, fit)
    prefix = str(op.get("id") or new_id())
    ids: dict[int, str] = {}
    parent = str(op["parent"]) if op.get("parent") else None
    new_layers: list[Layer] = []
    mask_size = (max(1, round(page.spec.width_mm / 25.4 * MASK_DPI)), max(1, round(page.spec.height_mm / 25.4 * MASK_DPI)))
    for i, item in enumerate(layers):
        ids[i] = f"{prefix}-{i + 1}"
    for i, item in enumerate(layers):
        layer = Layer(id=ids[i], role=LayerRole.USER, title=item.name[:80], visible=item.visible,
                      opacity=round(max(0.0, min(1.0, item.opacity)), 3), blend=item.blend, clip=item.clip, exportable=True,
                      parent_id=ids[item.parent] if item.parent is not None else parent)
        if item.folder:
            layer.kind = LayerKind.FOLDER
        else:
            canvas = Image.new("RGBA", page_px, (0, 0, 0, 0))
            picture = item.image if shown == doc.size else item.image.resize(shown, Image.LANCZOS)
            canvas.paste(picture, at)  # (onto a clear layer: the pixels as they are)
            layer.kind = LayerKind.RASTER
            layer.source = {"kind": "psd"}  # (a painting app's picture: on a monochrome page it prints in grey and tones)
            layer.raster_png = _png(canvas)
            layer.raster_relpath = f"pages/{page.index:03d}/user-{layer.id}.png"
            if item.mask is not None:
                full = Image.new("L", page_px, 255)
                full.paste(item.mask.resize(shown), at)
                layer.mask = {"png": _png(full.resize(mask_size)), "enabled": True}
        new_layers.append(layer)
    known = {item.id for item in page.layers}
    if any(layer.id in known for layer in new_layers):
        raise ApplyError(f"layer {prefix} exists")
    after = op.get("after")
    index = next((i + 1 for i, item in enumerate(page.layers) if item.id == after), len(page.layers)) if after else len(page.layers)
    page.layers[index:index] = new_layers
    op["_report"] = {"layers": [{"id": layer.id, "title": layer.title} for layer in new_layers], "skipped": list(doc.skipped)}


def set_timelapse(episode, op: dict) -> None:
    """Record the work as it goes (a small picture of each changed page at every save), or stop."""
    on = bool(op.get("on", True))
    if on:
        episode.extra["timelapse"] = {"on": True}
    else:
        episode.extra.pop("timelapse", None)


def apply(episode, op: dict[str, Any], name: str) -> None:
    {"import_psd": import_psd, "set_timelapse": set_timelapse}[name](episode, op)
