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

    @classmethod
    def from_dict(cls, data: dict | None) -> "LineParams":
        data = data or {}
        radius = int(data.get("radius", cls.radius))
        return cls(radius=radius if radius % 2 else radius + 1, threshold=float(data.get("threshold", cls.threshold)),
                   min_px=int(data.get("min_px", cls.min_px)))


def _drop_specks(mask: Image.Image, min_px: int) -> Image.Image:
    """Remove 255-components with fewer than min_px pixels."""
    if min_px <= 1:
        return mask
    w, h = mask.size
    data = bytearray(mask.tobytes())
    seen = bytearray(w * h)
    for start in range(w * h):
        if not data[start] or seen[start]:
            continue
        stack = [start]
        seen[start] = 1
        component = []
        while stack:
            i = stack.pop()
            component.append(i)
            y, x = divmod(i, w)
            for dy in (-1, 0, 1):
                ny = y + dy
                if ny < 0 or ny >= h:
                    continue
                for dx in (-1, 0, 1):
                    nx = x + dx
                    if 0 <= nx < w:
                        j = ny * w + nx
                        if data[j] and not seen[j]:
                            seen[j] = 1
                            stack.append(j)
        if len(component) < min_px:
            for i in component:
                data[i] = 0
    return Image.frombytes("L", (w, h), bytes(data))


def extract(image: Image.Image, params: LineParams | None = None) -> Image.Image:
    params = params or LineParams()
    grey = image.convert("L")
    local_max = grey.filter(ImageFilter.MaxFilter(params.radius))
    # ratio = L / local max, scaled to 0..255: 255 on flat areas (light or dark), low on strokes
    ratio_data = bytes(
        255 if m == 0 else min(255, v * 255 // m) for v, m in zip(grey.tobytes(), local_max.tobytes())
    )
    ratio = Image.frombytes("L", grey.size, ratio_data)
    cut = round(params.threshold * 255)
    ink = ratio.point(lambda v: 255 if v < cut else 0)
    # solid blacks (beta) are not lines, but keep them: a dark region whose local max is dark too
    solid = ImageChops.multiply(grey.point(lambda v: 255 if v < 40 else 0), local_max.point(lambda v: 255 if v < 80 else 0))
    ink = ImageChops.lighter(ink, solid)
    ink = _drop_specks(ink, params.min_px)
    out = Image.new("RGBA", grey.size, (0, 0, 0, 0))
    out.putalpha(ink)
    return out


def to_png(layer: Image.Image) -> bytes:
    import io

    buf = io.BytesIO()
    layer.save(buf, format="PNG")
    return buf.getvalue()
