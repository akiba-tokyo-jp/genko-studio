"""Test images for the offline pipeline (origin.kind = "fixture"). Made with Pillow, never shipped."""

import io
import json

from PIL import Image, ImageDraw, PngImagePlugin

SENTINEL = (255, 0, 255)  # decoy candidates are magenta: it must never reach a printed page


def _png(image: Image.Image, faces: list) -> bytes:
    info = PngImagePlugin.PngInfo()
    info.add_text("faces", json.dumps(faces))
    buf = io.BytesIO()
    image.save(buf, format="PNG", pnginfo=info)
    return buf.getvalue()


def panel(px: list[int], figures: list[dict], shade: int = 0) -> bytes:
    """Grey gradient ground, a grey body and a white head (outlined) where the request's pose guide puts them."""
    w, h = int(px[0]), int(px[1])
    image = Image.new("L", (w, h), 230)
    draw = ImageDraw.Draw(image)
    for y in range(0, h, 8):
        draw.rectangle((0, y, w, y + 8), fill=150 + int(80 * y / h) - shade)
    faces = []
    for fig in figures:
        bx, by, bw, bh = fig["body01"]
        draw.rectangle((bx * w, by * h, (bx + bw) * w, (by + bh) * h), fill=90)
        hx, hy, hw, hh = fig["head01"]
        draw.ellipse((hx * w, hy * h, (hx + hw) * w, (hy + hh) * h), fill=250, outline=0, width=max(2, w // 200))
        faces.append({"char": fig["char"], "box01": fig["head01"]})
    return _png(image.convert("RGB"), faces)


def decoy(px: list[int]) -> bytes:
    w, h = int(px[0]) // 2, int(px[1]) // 2
    return _png(Image.new("RGB", (max(64, w), max(64, h)), SENTINEL), [])


def sheet(px: list[int]) -> bytes:
    """A standing figure with the face close-up at the top centre (where approve sheet cuts the face)."""
    w, h = int(px[0]), int(px[1])
    image = Image.new("L", (w, h), 245)
    draw = ImageDraw.Draw(image)
    side = int(0.4 * w)
    draw.ellipse((0.3 * w, 0.03 * h, 0.3 * w + side, 0.03 * h + side), fill=255, outline=0, width=4)
    draw.rectangle((0.35 * w, 0.03 * h + side + 20, 0.65 * w, 0.95 * h), fill=120)
    return _png(image.convert("RGB"), [{"box01": [0.3, 0.03, 0.4, side / h]}])
