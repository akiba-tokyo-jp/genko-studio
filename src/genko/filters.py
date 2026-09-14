from __future__ import annotations

from PIL import Image, ImageEnhance, ImageFilter, ImageOps


def apply_filter(image: Image.Image, kind: str, params: dict | None = None) -> Image.Image:
    params = params or {}
    rgba = image.convert("RGBA")
    if kind == "blur":
        radius = float(params.get("radius", 2))
        return rgba.filter(ImageFilter.GaussianBlur(radius=radius))
    if kind == "sharpen":
        rgb = rgba.convert("RGB").point(lambda p: min(255, p + 12))
        rgb = ImageEnhance.Contrast(rgb).enhance(1.3)
        rgb = rgb.filter(ImageFilter.UnsharpMask(radius=2, percent=150, threshold=3))
        out = rgb.convert("RGBA")
        out.putalpha(rgba.split()[3])
        return out
    if kind == "hue":
        shift = int(params.get("shift", 30)) % 360
        h, s, v = rgba.convert("RGB").convert("HSV").split()
        h = h.point(lambda p: (p + round(shift * 255 / 360)) % 256)
        rgb = Image.merge("HSV", (h, s, v)).convert("RGB")
        out = rgb.convert("RGBA")
        out.putalpha(rgba.split()[3])
        return out
    if kind == "levels":
        black = int(params.get("black", 0))
        white = int(params.get("white", 255))
        span = max(1, white - black)

        def _map(p: int) -> int:
            if p <= black:
                return 0
            if p >= white:
                return 255
            return round((p - black) * 255 / span)

        rgb = rgba.convert("RGB").point(_map)
        out = rgb.convert("RGBA")
        out.putalpha(rgba.split()[3])
        return out
    if kind == "curve":
        gamma = float(params.get("gamma", 1.6))
        rgb = ImageOps.autocontrast(rgba.convert("RGB"))
        rgb = ImageEnhance.Brightness(rgb).enhance(1 / max(0.2, gamma))
        out = rgb.convert("RGBA")
        out.putalpha(rgba.split()[3])
        return out
    if kind == "mosaic":
        block = max(2, int(params.get("block", 8)))
        w, h = rgba.size
        small = rgba.resize((max(1, w // block), max(1, h // block)), Image.Resampling.NEAREST)
        out = small.resize((w, h), Image.Resampling.NEAREST)
        rgb = ImageEnhance.Brightness(out.convert("RGB")).enhance(0.88)
        result = rgb.convert("RGBA")
        result.putalpha(rgba.split()[3])
        return result
    if kind == "bitonal":
        threshold = int(params.get("threshold", 180))
        gray = ImageOps.grayscale(rgba)
        bw = gray.point(lambda p: 255 if p > threshold else 0)
        out = bw.convert("RGBA")
        out.putalpha(rgba.split()[3])
        return out
    raise ValueError(f"unknown filter {kind}")
