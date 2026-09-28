"""Filters on a layer's pixels (and, J5, the adjustments of correction layers): blur, sharpen, hue,
levels, curve, mosaic, bitonal, and motion / radial / zoom blur, noise, wave, twirl, line extraction,
invert, posterize, threshold, gradient map.
"""

from __future__ import annotations

import math
import random

from PIL import Image, ImageChops, ImageEnhance, ImageFilter, ImageOps

KINDS = ("blur", "sharpen", "hue", "levels", "curve", "mosaic", "bitonal", "motion_blur", "radial_blur", "zoom_blur", "noise",
         "wave", "twirl", "lineart", "invert", "posterize", "threshold", "gradient_map", "brightness_contrast", "despeckle", "glow", "rain")
# the ones a correction layer (調整レイヤー) can hold: they change colours, not shapes
ADJUSTMENTS = ("levels", "curve", "hue", "invert", "posterize", "threshold", "gradient_map", "bitonal", "brightness_contrast")
CHANNELS = ("rgb", "r", "g", "b")


def _by_channel(rgba: Image.Image, table: list[int], channel: str) -> Image.Image:
    """A 256-entry table through all three colours, or one of them (channel r, g or b)."""
    channel = channel if channel in CHANNELS else "rgb"
    if channel == "rgb":
        return _keep_alpha(rgba.convert("RGB").point(table * 3), rgba)
    identity = list(range(256))
    tables = [table if name == channel else identity for name in ("r", "g", "b")]
    return _keep_alpha(rgba.convert("RGB").point(tables[0] + tables[1] + tables[2]), rgba)


def curve_table(points) -> list[int]:
    """トーンカーブ: a smooth curve through the points ([[in, out], …], 0..255) that never turns back
    (monotone cubic, Fritsch–Carlson); the ends run flat beyond the first and last point."""
    pts = sorted({int(round(float(x))): float(y) for x, y in points}.items())
    if len(pts) < 2:
        raise ValueError("a tone curve needs two points or more")
    xs = [float(x) for x, _ in pts]
    ys = [max(0.0, min(255.0, y)) for _, y in pts]
    n = len(xs)
    d = [(ys[k + 1] - ys[k]) / max(1e-9, xs[k + 1] - xs[k]) for k in range(n - 1)]
    m = [d[0]] + [0.0 if d[k - 1] * d[k] <= 0 else (d[k - 1] + d[k]) / 2 for k in range(1, n - 1)] + [d[-1]]
    for k in range(n - 1):
        if d[k] == 0:
            m[k] = m[k + 1] = 0.0
            continue
        a, b = m[k] / d[k], m[k + 1] / d[k]
        if a * a + b * b > 9:
            t = 3 / math.sqrt(a * a + b * b)
            m[k], m[k + 1] = t * a * d[k], t * b * d[k]
    table = []
    for i in range(256):
        if i <= xs[0]:
            table.append(round(ys[0]))
            continue
        if i >= xs[-1]:
            table.append(round(ys[-1]))
            continue
        k = next(j for j in range(n - 1) if xs[j] <= i <= xs[j + 1])
        h = xs[k + 1] - xs[k]
        t = (i - xs[k]) / h
        h00, h10, h01, h11 = 2 * t ** 3 - 3 * t ** 2 + 1, t ** 3 - 2 * t ** 2 + t, -2 * t ** 3 + 3 * t ** 2, t ** 3 - t ** 2
        value = h00 * ys[k] + h10 * h * m[k] + h01 * ys[k + 1] + h11 * h * m[k + 1]
        table.append(max(0, min(255, round(value))))
    return table


def levels_table(black: float = 0, white: float = 255, gamma: float = 1.0, out_black: float = 0, out_white: float = 255) -> list[int]:
    """レベル補正: input black and white points, the middle (gamma: > 1 lightens the middle greys, as CLIP's
    middle slider moved left) and the output range."""
    black, white = float(black), max(float(black) + 1, float(white))
    gamma = max(0.1, min(9.99, float(gamma)))
    table = []
    for p in range(256):
        t = min(1.0, max(0.0, (p - black) / (white - black)))
        t = t ** (1 / gamma)
        table.append(max(0, min(255, round(float(out_black) + t * (float(out_white) - float(out_black))))))
    return table


def brightness_contrast_table(brightness: float = 0, contrast: float = 0) -> list[int]:
    """明るさ・コントラスト (each -100..100): brightness moves every value, contrast pulls them from or toward
    the middle grey."""
    b = max(-100.0, min(100.0, float(brightness))) * 1.275
    c = max(-100.0, min(100.0, float(contrast)))
    k = 1 + c / 100 if c <= 0 else 1 / max(0.01, 1 - c / 100 * 0.99)
    return [max(0, min(255, round((p - 127.5) * k + 127.5 + b))) for p in range(256)]


def _components(mask):
    """Connected parts (8-neighbour) of a bool array as runs: (runs [(y, x0, x1)], label per run, size per label).
    Rows are cut into runs and runs that touch across rows are joined, so a page of lines is quick."""
    import numpy as np

    runs: list[tuple[int, int, int]] = []
    parent: list[int] = []

    def find(i: int) -> int:
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    previous: list[int] = []
    for y in range(mask.shape[0]):
        row = mask[y]
        if not row.any():
            previous = []
            continue
        edges = np.diff(np.concatenate(([0], row.view(np.uint8), [0])).astype(np.int8))
        starts, ends = np.flatnonzero(edges == 1), np.flatnonzero(edges == -1)
        current = []
        j = 0
        for x0, x1 in zip(starts.tolist(), ends.tolist()):
            index = len(runs)
            runs.append((y, x0, x1))
            parent.append(index)
            current.append(index)
            while j < len(previous) and runs[previous[j]][2] < x0:  # (ends before this run's reach)
                j += 1
            k = j
            while k < len(previous) and runs[previous[k]][1] <= x1:  # (8-neighbour: touching at a corner counts)
                a, b = find(index), find(previous[k])
                if a != b:
                    parent[a] = b
                k += 1
        previous = current
    labels = [find(i) for i in range(len(runs))]
    sizes: dict[int, int] = {}
    for (y, x0, x1), label in zip(runs, labels):
        sizes[label] = sizes.get(label, 0) + x1 - x0
    return runs, labels, sizes


def despeckle(rgba: Image.Image, size_px: float, what: str = "ink") -> Image.Image:
    """ゴミ取り: specks (ink smaller than size_px across) taken away, or (what="holes") small holes in the ink
    filled; what="both" does both. On a see-through layer the specks become transparent, on a scan (paper
    everywhere) they become paper."""
    import numpy as np

    arr = np.array(rgba.convert("RGBA"))
    alpha = arr[..., 3]
    lum = arr[..., :3].astype(np.float32) @ np.array([0.299, 0.587, 0.114], dtype=np.float32)
    ink = (alpha >= 128) & (lum < 128)
    limit = max(1.0, float(size_px)) ** 2
    opaque = float((alpha > 250).mean()) > 0.9
    for part in (("ink", "holes") if what == "both" else (what,)):
        target = ink if part == "ink" else ~ink
        runs, labels, sizes = _components(target)
        for (y, x0, x1), label in zip(runs, labels):
            if sizes[label] >= limit:
                continue
            if part == "ink":
                arr[y, x0:x1] = (255, 255, 255, 255) if opaque else (0, 0, 0, 0)
            else:
                arr[y, x0:x1] = (0, 0, 0, 255)
        ink = (arr[..., 3] >= 128) & ((arr[..., :3].astype(np.float32) @ np.array([0.299, 0.587, 0.114], dtype=np.float32)) < 128)
    return Image.fromarray(arr, "RGBA")


def within(original: Image.Image, filtered: Image.Image, mask: Image.Image) -> Image.Image:
    """The filter only inside the selection: `mask` (L, the layer's size) chooses the filtered pixels."""
    return Image.composite(filtered.convert("RGBA"), original.convert("RGBA"), mask.convert("L").resize(original.size))


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
    if kind.startswith("plugin:"):  # (a filter a person installed: genko.plugins)
        from genko import plugins

        return plugins.run(kind, rgba, params)
    if kind == "blur":
        radius = float(params.get("radius", 2))
        return rgba.filter(ImageFilter.GaussianBlur(radius=radius))
    if kind == "sharpen":
        if params.get("amount") is not None:  # (シャープの強さ: 1 is the usual, 2 the strong one)
            amount = max(0.1, min(5.0, float(params["amount"])))
            rgb = rgba.convert("RGB").filter(ImageFilter.UnsharpMask(radius=max(0.5, float(params.get("radius", 2))),
                                                                      percent=round(120 * amount), threshold=2))
            return _keep_alpha(rgb, rgba)
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
    if kind == "brightness_contrast":
        table = brightness_contrast_table(params.get("brightness", 0), params.get("contrast", 0))
        return _by_channel(rgba, table, str(params.get("channel") or "rgb"))
    if kind == "glow":  # 光彩拡散: the bright parts spread out in a soft light, added over the picture
        radius = max(0.5, float(params.get("radius", 12)))
        amount = max(0.0, min(3.0, float(params.get("amount", 0.8))))
        cut = int(params.get("threshold", 170))
        rgb = rgba.convert("RGB")
        bright = ImageOps.grayscale(rgb).point(lambda v: 255 if v >= cut else 0)
        light = Image.composite(rgb, Image.new("RGB", rgb.size, (0, 0, 0)), bright).filter(ImageFilter.GaussianBlur(radius))
        glow = ImageEnhance.Brightness(light).enhance(amount)
        return _keep_alpha(ImageChops.screen(rgb, glow), rgba)
    if kind == "rain":  # 雨: slanted streaks all over, in the given colour
        from PIL import ImageDraw

        rng = random.Random(str(params.get("seed", "rain")))
        w, h = rgba.size
        count = max(1, min(20000, int(float(params.get("count", 400)))))
        length = max(2.0, float(params.get("length", 40)))
        width = max(1, round(float(params.get("width", 1))))
        angle = math.radians(90 + float(params.get("angle", 15)))  # (0: straight down; + leans to the right)
        colour = tuple(int(v) for v in (params.get("rgb") or [255, 255, 255]))[:3]
        opacity = max(0.0, min(1.0, float(params.get("opacity", 0.7))))
        layer = Image.new("RGBA", rgba.size, (0, 0, 0, 0))
        draw = ImageDraw.Draw(layer)
        dx, dy = math.cos(angle), math.sin(angle)
        for _ in range(count):
            x, y = rng.uniform(-length, w + length), rng.uniform(-length, h)
            size = length * (0.5 + rng.random())
            draw.line([(x, y), (x - dx * size, y + dy * size)], fill=(*colour, round(255 * opacity * (0.5 + 0.5 * rng.random()))), width=width)
        out = rgba.copy()
        out.alpha_composite(layer)
        out.putalpha(ImageChops.lighter(rgba.split()[3], layer.split()[3]))
        return out
    if kind == "despeckle":
        dpi = float(params.get("dpi", 200))
        size = float(params["size_px"]) if params.get("size_px") is not None else float(params.get("size_mm", 0.3)) / 25.4 * dpi
        what = str(params.get("what") or "ink")
        if what not in ("ink", "holes", "both"):
            raise ValueError("despeckle what must be ink, holes or both")
        return despeckle(rgba, size, what)
    if kind == "levels" and any(params.get(k) is not None for k in ("gamma", "out_black", "out_white", "channel")):
        table = levels_table(params.get("black", 0), params.get("white", 255), params.get("gamma", 1.0),
                             params.get("out_black", 0), params.get("out_white", 255))
        return _by_channel(rgba, table, str(params.get("channel") or "rgb"))
    if kind == "curve" and params.get("points"):
        return _by_channel(rgba, curve_table(params["points"]), str(params.get("channel") or "rgb"))
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
        if params.get("stops"):  # (each colour at its own place: [[position 0..1, [r,g,b]], …])
            import numpy as np

            placed = sorted((float(st[0]), [int(v) for v in st[1]][:3]) for st in params["stops"])
            if len(placed) < 2:
                raise ValueError("a gradient map needs two colours or more")
            xs = np.linspace(0, 1, 256)
            table = [[round(v) for v in np.interp(xs, [p for p, _ in placed], [c[ch] for _, c in placed])] for ch in range(3)]
            grey = ImageOps.grayscale(rgba)
            return _keep_alpha(Image.merge("RGB", [grey.point(table[ch]) for ch in range(3)]), rgba)
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
