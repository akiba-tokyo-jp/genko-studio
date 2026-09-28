"""Line extraction for generated images (Pillow only).

1. Divide the luminance by its local maximum (MaxFilter): flat shading and
   gradients go to white, dark strokes stay dark.
2. Threshold the result.
3. Drop specks smaller than `min_px` (connected components, 8-neighbour).
4. Return an RGBA ink raster: black lines, transparent elsewhere.
"""

from __future__ import annotations

from dataclasses import dataclass

from PIL import Image, ImageChops, ImageFilter


@dataclass(frozen=True)
class LineParams:
    radius: int = 7          # MaxFilter size ~ twice the widest stroke, in source pixels (odd)
    threshold: float = 0.72  # L / local max below this is ink
    min_px: int = 12         # specks smaller than this are dropped
    drop_blue: bool = False  # 水色の下描き（青鉛筆）を消す: bluish marks are paper
    keep_solid: bool = True  # ベタ（塗りつぶした黒）を残す
    rgb: tuple = (0, 0, 0)   # the lines' colour

    @classmethod
    def from_dict(cls, data: dict | None) -> "LineParams":
        data = data or {}
        radius = int(data.get("radius", cls.radius))
        rgb = data.get("rgb") or cls.rgb
        return cls(radius=max(3, radius if radius % 2 else radius + 1), threshold=max(0.05, min(0.99, float(data.get("threshold", cls.threshold)))),
                   min_px=max(0, int(data.get("min_px", cls.min_px))), drop_blue=bool(data.get("drop_blue", cls.drop_blue)),
                   keep_solid=bool(data.get("keep_solid", cls.keep_solid)), rgb=tuple(int(v) for v in list(rgb)[:3]))


def _drop_specks(mask: Image.Image, min_px: int) -> Image.Image:
    """Remove 255-components with fewer than min_px pixels."""
    if min_px <= 1:
        return mask
    import numpy as np

    from genko.filters import _components

    arr = np.array(mask.convert("L"))
    runs, labels, sizes = _components(arr > 0)
    for (y, x0, x1), label in zip(runs, labels):
        if sizes[label] < min_px:
            arr[y, x0:x1] = 0
    return Image.fromarray(arr, "L")


def extract(image: Image.Image, params: LineParams | None = None) -> Image.Image:
    """線画抽出: the lines of a scan or a picture as ink on a transparent layer."""
    import numpy as np

    params = params or LineParams()
    rgb = image.convert("RGB")
    grey = rgb.convert("L")
    if params.drop_blue:  # (水色の下描き: where blue stands clearly above red, the mark is taken as paper)
        a = np.asarray(rgb, dtype=np.int16)
        bluish = (a[..., 2] - a[..., 0] > 30) & (a[..., 2] > 90)
        g = np.asarray(grey).copy()
        g[bluish] = 255
        grey = Image.fromarray(g, "L")
    local_max = grey.filter(ImageFilter.MaxFilter(params.radius))
    g = np.asarray(grey, dtype=np.float32)
    m = np.asarray(local_max, dtype=np.float32)
    ratio = np.where(m == 0, 1.0, g / np.maximum(m, 1.0))  # 1 on flat areas (light or dark), low on strokes
    ink = Image.fromarray(np.where(ratio < params.threshold, 255, 0).astype(np.uint8), "L")
    if params.keep_solid:  # (solid blacks are not lines, but keep them: a dark region whose local max is dark too)
        solid = ImageChops.multiply(grey.point(lambda v: 255 if v < 40 else 0), local_max.point(lambda v: 255 if v < 80 else 0))
        ink = ImageChops.lighter(ink, solid)
    ink = _drop_specks(ink, params.min_px)
    out = Image.new("RGBA", grey.size, (*params.rgb, 0))
    out.putalpha(ink)
    return out


def to_png(layer: Image.Image) -> bytes:
    import io

    buf = io.BytesIO()
    layer.save(buf, format="PNG")
    return buf.getvalue()
