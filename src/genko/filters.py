"""Filters on a layer's pixels (and, J5, the adjustments of correction layers): blur, sharpen, hue,
levels, curve, mosaic, bitonal, and motion / radial / zoom blur, noise, wave, twirl, line extraction,
invert, posterize, threshold, gradient map.
"""

from __future__ import annotations

import math
import random

from PIL import Image, ImageChops, ImageEnhance, ImageFilter, ImageOps

KINDS = ("blur", "sharpen", "hue", "levels", "curve", "mosaic", "bitonal", "motion_blur", "radial_blur", "zoom_blur", "noise",
         "wave", "twirl", "lineart", "invert", "posterize", "threshold", "gradient_map")
# the ones a correction layer (調整レイヤー) can hold: they change colours, not shapes
ADJUSTMENTS = ("levels", "curve", "hue", "invert", "posterize", "threshold", "gradient_map", "bitonal")


def _keep_alpha(rgb: Image.Image, source: Image.Image) -> Image.Image:
    out = rgb.convert("RGBA")
    out.putalpha(source.split()[3])
    return out


def _numpy(image: Image.Image):
    import numpy as np

    return np.asarray(image, dtype=np.float32)


def _remap(rgba: Image.Image, map_x, map_y) -> Image.Image:
    """Pixels fetched from (map_x, map_y) for each place (bilinear; outside: transparent)."""
    import numpy as np

    src = _numpy(rgba)
    h, w = src.shape[:2]
    x0 = np.floor(map_x).astype(np.int64)
    y0 = np.floor(map_y).astype(np.int64)
    fx, fy = (map_x - x0)[..., None], (map_y - y0)[..., None]

    def at(yy, xx):
        inside = (xx >= 0) & (xx < w) & (yy >= 0) & (yy < h)
        out = src[np.clip(yy, 0, h - 1), np.clip(xx, 0, w - 1)]
        return out * inside[..., None]

    top = at(y0, x0) * (1 - fx) + at(y0, x0 + 1) * fx
    bottom = at(y0 + 1, x0) * (1 - fx) + at(y0 + 1, x0 + 1) * fx
    mixed = top * (1 - fy) + bottom * fy
    return Image.fromarray(np.clip(mixed + 0.5, 0, 255).astype(np.uint8), "RGBA")


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
        shift = int(float(params.get("shift", 30))) % 360
        sat = max(0.0, float(params.get("saturation", 1.0)))
        val = max(0.0, float(params.get("value", 1.0)))
        h, s, v = rgba.convert("RGB").convert("HSV").split()
        h = h.point(lambda p: (p + round(shift * 255 / 360)) % 256)
        s = s.point(lambda p: min(255, round(p * sat)))
        v = v.point(lambda p: min(255, round(p * val)))
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
        # a gamma curve: > 1 darkens the middle tones, < 1 lightens them; black and white stay
        gamma = max(0.2, min(5.0, float(params.get("gamma", 1.6))))
        table = [round(255 * (i / 255) ** gamma) for i in range(256)]
        rgb = rgba.convert("RGB").point(table * 3)
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
    if kind == "motion_blur":
        # 移動ぼかし: the picture smeared along one direction
        distance = max(1, int(params.get("distance", 12)))
        angle = math.radians(float(params.get("angle", 0)))
        steps = max(2, min(64, distance))
        acc = None
        for k in range(steps):
            t = (k / (steps - 1) - 0.5) * distance
            shifted = ImageChops.offset(rgba, round(t * math.cos(angle)), round(t * math.sin(angle)))
            acc = shifted if acc is None else Image.blend(acc, shifted, 1 / (k + 1))
        return acc
    if kind in ("radial_blur", "zoom_blur"):
        # 放射ぼかし (turning about the centre) and ズーム (toward it)
        cx = float(params.get("cx", 0.5)) * rgba.width
        cy = float(params.get("cy", 0.5)) * rgba.height
        amount = max(0.0, float(params.get("amount", 0.08)))
        steps = 10
        acc = None
        for k in range(steps):
            t = (k / (steps - 1) - 0.5) * amount
            if kind == "radial_blur":
                moved = rgba.rotate(math.degrees(t), resample=Image.Resampling.BILINEAR, center=(cx, cy))
            else:
                scale = 1 + t
                moved = rgba.transform(rgba.size, Image.Transform.AFFINE,
                                       (1 / scale, 0, cx - cx / scale, 0, 1 / scale, cy - cy / scale), Image.Resampling.BILINEAR)
            acc = moved if acc is None else Image.blend(acc, moved, 1 / (k + 1))
        return acc
    if kind == "noise":
        amount = max(0.0, min(1.0, float(params.get("amount", 0.15))))
        rng = random.Random(str(params.get("seed", "genko")))

        def grain() -> Image.Image:
            return Image.frombytes("L", rgba.size, bytes(rng.randrange(256) for _ in range(rgba.width * rgba.height)))

        rgb = rgba.convert("RGB")
        if params.get("mono", True):
            g = grain()
            noisy = Image.blend(rgb, Image.merge("RGB", (g, g, g)), amount)
        else:  # colour noise: each channel its own grain
            noisy = Image.merge("RGB", [Image.blend(band, grain(), amount) for band in rgb.split()])
        return _keep_alpha(noisy, rgba)
    if kind in ("wave", "twirl"):
        import numpy as np

        h, w = rgba.height, rgba.width
        ys, xs = np.mgrid[0:h, 0:w].astype(np.float32)
        if kind == "wave":
            amplitude = float(params.get("amplitude", 6))
            wavelength = max(2.0, float(params.get("wavelength", 60)))
            map_x = xs + amplitude * np.sin(ys * 2 * math.pi / wavelength)
            map_y = ys + amplitude * np.sin(xs * 2 * math.pi / wavelength)
        else:
            cx, cy = w / 2, h / 2
            radius = max(1.0, float(params.get("radius", 0.45)) * min(w, h))
            twist = math.radians(float(params.get("angle", 90)))
            dx, dy = xs - cx, ys - cy
            dist = np.sqrt(dx * dx + dy * dy)
            turn = twist * np.clip(1 - dist / radius, 0, 1) ** 2
            cos_t, sin_t = np.cos(turn), np.sin(turn)
            map_x = cx + dx * cos_t - dy * sin_t
            map_y = cy + dx * sin_t + dy * cos_t
        return _remap(rgba, map_x, map_y)
    if kind == "lineart":
        from genko.lineart import LineParams, extract

        white = Image.new("RGBA", rgba.size, (255, 255, 255, 255))
        white.alpha_composite(rgba)
        return extract(white, LineParams.from_dict(params))
    if kind == "invert":
        return _keep_alpha(ImageOps.invert(rgba.convert("RGB")), rgba)
    if kind == "posterize":
        levels = max(2, min(64, int(params.get("levels", 4))))
        step = 255 / (levels - 1)
        table = [round(round(i / step) * step) for i in range(256)]
        return _keep_alpha(rgba.convert("RGB").point(table * 3), rgba)
    if kind == "threshold":
        cut = int(params.get("threshold", 128))
        grey = ImageOps.grayscale(rgba).point(lambda p: 255 if p >= cut else 0)
        return _keep_alpha(grey.convert("RGB"), rgba)
    if kind == "gradient_map":
        # the picture's lightness mapped to a row of colours (dark → first)
        stops = [tuple(int(v) for v in c)[:3] for c in params.get("colors") or [[0, 0, 0], [255, 255, 255]]]
        if len(stops) < 2:
            raise ValueError("a gradient map needs two colours or more")
        table = [[], [], []]
        for i in range(256):
            pos = i / 255 * (len(stops) - 1)
            k = min(len(stops) - 2, int(pos))
            t = pos - k
            for c in range(3):
                table[c].append(round(stops[k][c] + (stops[k + 1][c] - stops[k][c]) * t))
        grey = ImageOps.grayscale(rgba)
        mapped = Image.merge("RGB", [grey.point(table[c]) for c in range(3)])
        return _keep_alpha(mapped, rgba)
    raise ValueError(f"unknown filter {kind}")
