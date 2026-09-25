"""Pen and brush kinds: how a vector line is drawn, and the presets people pick.

Every line keeps its points (with pressure) and its `kind`; the look is made at render time, at the
render's resolution, so a line can be redrawn in another kind and prints sharp at 600 dpi.
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass

from PIL import Image, ImageChops, ImageDraw, ImageFilter

from genko.stroke import draw_stroke_mm


@dataclass(frozen=True)
class Brush:
    key: str
    label: str
    width_mm: float  # the default size
    min_pressure: float = 0.15  # how thin a light touch gets (share of the width)
    gamma: float = 1.0  # pressure curve: >1 needs more force for a thick line
    opacity: float = 1.0
    stabilize: int = 3
    taper: bool = True
    texture: str = ""  # "" | grain (pencil) | soft (airbrush) | dry (brush)
    rgb: tuple[int, int, int] | None = None  # a fixed colour (white)
    fixed_width: bool = False  # ignores pressure (technical pens, markers)
    # --- J3: the tip and how it is laid down ---
    tip: str = "round"  # round | flat (a chisel: calligraphy) | image (tip_png) | a pattern's own shape
    tip_angle: float = 45.0  # degrees (flat tips)
    tip_ratio: float = 0.25  # thin side / wide side (flat tips)
    tip_follow: bool = False  # the tip turns with the line's direction
    tip_png: str = ""  # base64 grey picture: the stamp (image tips, made from a picture or an .abr)
    spacing: float = 0.0  # stamps at this share of the width apart (0: a continuous line)
    scatter: float = 0.0  # stamps thrown this share of the width off the line (spray, stipple)
    size_jitter: float = 0.0  # stamps vary in size by up to this share
    turn_jitter: bool = False  # stamps turned at random
    count: int = 1  # stamps at each step
    pattern: str = ""  # dots | dash | lace | grass | hearts | stars | leaves (模様ブラシ)
    speed: float = 0.0  # 0..1: the quicker the hand, the thinner the line (速度)
    post_smooth: int = 0  # 0..10: the line is smoothed after it is drawn (後補正)
    aa: str = "normal"  # none | weak | normal | strong (アンチエイリアス)
    stamp_size: float = 1.0  # each stamp's size as a share of the width (spray drops are small)


BRUSHES: dict[str, Brush] = {b.key: b for b in (
    Brush("gpen", "G ペン", 0.5, min_pressure=0.1, gamma=1.4, stabilize=3, taper=True),
    Brush("maru", "丸ペン", 0.25, min_pressure=0.2, gamma=1.2, stabilize=3, taper=True),
    Brush("kabura", "かぶらペン", 0.6, min_pressure=0.35, gamma=1.0, stabilize=2, taper=True),
    Brush("mili", "ミリペン", 0.3, fixed_width=True, stabilize=4, taper=False),
    Brush("pencil", "鉛筆", 0.5, min_pressure=0.4, gamma=1.0, opacity=0.85, stabilize=1, taper=False, texture="grain"),
    Brush("fude", "筆", 1.6, min_pressure=0.05, gamma=1.8, stabilize=2, taper=True, texture="dry"),
    Brush("marker", "マーカー", 1.5, fixed_width=True, opacity=0.6, stabilize=2, taper=False),
    Brush("airbrush", "エアブラシ", 8.0, min_pressure=0.5, opacity=0.5, stabilize=1, taper=False, texture="soft"),
    Brush("fill_pen", "ベタ塗りペン", 3.0, fixed_width=True, stabilize=1, taper=False),
    Brush("white", "ホワイト（修正）", 1.0, min_pressure=0.3, stabilize=2, taper=False, rgb=(255, 255, 255)),
    Brush("fx", "効果線ペン", 0.5, min_pressure=0.0, gamma=1.0, stabilize=0, taper=False),
    # J3: brushes laid down in stamps, and the others CLIP STUDIO has
    Brush("calligraphy", "カリグラフィ（平たいペン先）", 1.6, min_pressure=0.5, stabilize=3, taper=False, tip="flat", tip_angle=35,
          tip_ratio=0.22),
    Brush("water", "水彩", 4.0, min_pressure=0.4, opacity=0.55, stabilize=2, taper=False, texture="water"),
    Brush("spray", "スプレー", 8.0, min_pressure=0.5, stabilize=1, taper=False, pattern="dots", spacing=0.08, scatter=0.5,
          size_jitter=0.6, count=6, stamp_size=0.06),
    Brush("stipple", "点描", 3.0, min_pressure=0.5, stabilize=1, taper=False, pattern="dots", spacing=0.35, scatter=0.45,
          size_jitter=0.5, count=2, stamp_size=0.2),
    Brush("dotline", "点線", 0.8, fixed_width=True, stabilize=4, taper=False, pattern="dots", spacing=2.2, stamp_size=1.0),
    Brush("dashline", "破線", 0.5, fixed_width=True, stabilize=4, taper=False, pattern="dash", spacing=5.0, stamp_size=1.0),
    Brush("lace", "レース", 3.0, fixed_width=True, stabilize=4, taper=False, pattern="lace", spacing=1.0, stamp_size=1.0),
    Brush("grass", "草むら", 6.0, min_pressure=0.5, stabilize=2, taper=False, pattern="grass", spacing=0.22, size_jitter=0.5,
          stamp_size=1.0),
    Brush("leaves", "木の葉", 5.0, min_pressure=0.5, stabilize=2, taper=False, pattern="leaves", spacing=0.45, scatter=0.6,
          size_jitter=0.5, turn_jitter=True, count=2, stamp_size=0.6),
    Brush("hearts", "ハート", 3.0, fixed_width=True, stabilize=3, taper=False, pattern="hearts", spacing=1.4, stamp_size=1.0),
    Brush("stars", "星", 3.0, fixed_width=True, stabilize=3, taper=False, pattern="stars", spacing=1.5, turn_jitter=True,
          size_jitter=0.3, stamp_size=1.0),
)}
DEFAULT = "gpen"
LEGACY = {"oil": "marker"}


# brushes people made: key → Brush. Filled from each book that carries them (Episode.brush_custom) and
# from the person's own library (the config folder's brushes.json), so a line drawn with one looks the
# same on any computer that opens the book.
CUSTOM: dict[str, Brush] = {}
TEXTURES = ("", "grain", "soft", "dry", "water")
TIPS = ("round", "flat", "image")
PATTERNS = ("", "dots", "dash", "lace", "grass", "hearts", "stars", "leaves")
AAS = ("none", "weak", "normal", "strong")
LIMITS = {"width_mm": (0.05, 50.0), "min_pressure": (0.0, 1.0), "gamma": (0.2, 5.0), "opacity": (0.05, 1.0), "stabilize": (0, 15),
          "tip_angle": (-360.0, 360.0), "tip_ratio": (0.02, 1.0), "spacing": (0.0, 5.0), "scatter": (0.0, 5.0),
          "size_jitter": (0.0, 1.0), "count": (1, 12), "speed": (0.0, 1.0), "post_smooth": (0, 10), "stamp_size": (0.02, 3.0)}


def brush(key: str | None) -> Brush:
    key = LEGACY.get(key or "", key or DEFAULT)
    return BRUSHES.get(key) or CUSTOM.get(key) or BRUSHES[DEFAULT]


def everything() -> dict[str, Brush]:
    return {**BRUSHES, **CUSTOM}


J3_KEYS = ("tip", "tip_angle", "tip_ratio", "tip_follow", "tip_png", "spacing", "scatter", "size_jitter", "turn_jitter", "count",
           "pattern", "speed", "post_smooth", "aa", "stamp_size")


def to_dict(b: Brush) -> dict:
    out = {"label": b.label, "width_mm": b.width_mm, "min_pressure": b.min_pressure, "gamma": b.gamma, "opacity": b.opacity,
           "stabilize": b.stabilize, "taper": b.taper, "texture": b.texture, "rgb": list(b.rgb) if b.rgb else None,
           "fixed_width": b.fixed_width}
    default = Brush("", "", 1.0)
    for key in J3_KEYS:  # (only what differs from a plain round pen: older books stay as they were)
        if getattr(b, key) != getattr(default, key):
            out[key] = getattr(b, key)
    return out


def from_dict(key: str, data: dict, base: str | None = None) -> Brush:
    """A brush from its settings (missing ones come from `base` or the G pen); ValueError when out of range."""
    start = to_dict(brush(base or data.get("base") or DEFAULT))
    default = Brush("", "", 1.0)
    start = {**{key: getattr(default, key) for key in J3_KEYS}, **start}
    merged = {**start, **{k: v for k, v in data.items() if k in start}}
    for name, (lo, hi) in LIMITS.items():
        value = float(merged[name])
        if not lo <= value <= hi:
            raise ValueError(f"brush {name} must be between {lo:g} and {hi:g}")
    if merged["texture"] not in TEXTURES:
        raise ValueError("brush texture must be none, grain, soft, dry or water")
    if merged["tip"] not in TIPS:
        raise ValueError("brush tip must be round, flat or image")
    if merged["pattern"] not in PATTERNS:
        raise ValueError("brush pattern must be dots, dash, lace, grass, hearts, stars or leaves")
    if merged["aa"] not in AAS:
        raise ValueError("brush aa must be none, weak, normal or strong")
    if merged["tip"] == "image" and not merged["tip_png"]:
        raise ValueError("an image tip needs its picture (tip_png)")
    label = str(merged["label"] or "").strip()
    if not label:
        raise ValueError("a brush needs a name")
    return Brush(key=key, label=label[:40], width_mm=float(merged["width_mm"]), min_pressure=float(merged["min_pressure"]),
                 gamma=float(merged["gamma"]), opacity=float(merged["opacity"]), stabilize=int(merged["stabilize"]),
                 taper=bool(merged["taper"]), texture=str(merged["texture"] or ""),
                 rgb=tuple(int(v) for v in merged["rgb"])[:3] if merged.get("rgb") else None,  # type: ignore[arg-type]
                 fixed_width=bool(merged["fixed_width"]), tip=str(merged["tip"]), tip_angle=float(merged["tip_angle"]),
                 tip_ratio=float(merged["tip_ratio"]), tip_follow=bool(merged["tip_follow"]), tip_png=str(merged["tip_png"] or ""),
                 spacing=float(merged["spacing"]), scatter=float(merged["scatter"]), size_jitter=float(merged["size_jitter"]),
                 turn_jitter=bool(merged["turn_jitter"]), count=int(merged["count"]), pattern=str(merged["pattern"] or ""),
                 speed=float(merged["speed"]), post_smooth=int(merged["post_smooth"]), aa=str(merged["aa"]),
                 stamp_size=float(merged["stamp_size"]))


def register(definitions: dict) -> None:
    """Make brushes known (a book's, or the library's); ones that do not make sense are skipped."""
    for key, data in (definitions or {}).items():
        if key in BRUSHES:
            continue
        try:
            CUSTOM[str(key)] = from_dict(str(key), dict(data))
        except (ValueError, TypeError, KeyError):
            continue


def library_path():
    from genko.tokens import config_dir

    return config_dir() / "brushes.json"


def load_library() -> dict:
    """The person's own brushes (key → settings), from the config folder."""
    import json

    path = library_path()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}
    return {str(k): dict(v) for k, v in (data.get("brushes") or {}).items()} if isinstance(data, dict) else {}


def save_to_library(key: str, data: dict | None) -> None:
    """Keep (or with None, forget) one of the person's brushes."""
    import json

    brushes_ = load_library()
    if data is None:
        brushes_.pop(key, None)
    else:
        brushes_[key] = data
    path = library_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"brushes": brushes_}, ensure_ascii=False, indent=1), encoding="utf-8")


def _pressured(points: list, b: Brush) -> list:
    out = []
    for pt in points:
        if b.fixed_width or len(pt) < 3:
            out.append((pt[0], pt[1], 1.0))
            continue
        p = max(0.0, min(1.0, float(pt[2]))) ** b.gamma
        out.append((pt[0], pt[1], b.min_pressure + (1 - b.min_pressure) * p))
    if b.speed > 0 and len(out) > 2:
        # the points come at a steady rate while drawing, so a long step means a quick hand: thinner
        import statistics

        steps = [math.dist(a[:2], c[:2]) for a, c in zip(out, out[1:])]
        usual = statistics.median(steps) or 1e-6
        thin = []
        for i, pt in enumerate(out):
            step = steps[min(i, len(steps) - 1)]
            quick = max(0.0, min(1.0, (step / usual - 1.0) / 3.0))
            thin.append((pt[0], pt[1], pt[2] * (1 - 0.75 * b.speed * quick)))
        out = thin
    return out


def smoothed(points: list, strength: int) -> list:
    """後補正: the line evened out after it is drawn (a moving average; the ends stay put)."""
    if strength <= 0 or len(points) < 4:
        return points
    window = max(1, int(strength))
    out = [points[0]]
    for i in range(1, len(points) - 1):
        lo, hi = max(0, i - window), min(len(points), i + window + 1)
        part = points[lo:hi]
        out.append(tuple([sum(float(p[0]) for p in part) / len(part), sum(float(p[1]) for p in part) / len(part)]
                         + list(points[i][2:3])))
    out.append(points[-1])
    return out


# --- stamps: image tips, flat tips, scattered and patterned brushes ---------------------------------------------------


def _decode_tip(data: str) -> Image.Image | None:
    import base64
    import io

    try:
        image = Image.open(io.BytesIO(base64.b64decode(data)))
    except Exception:  # (a broken picture: a round tip instead)
        return None
    if image.mode in ("RGBA", "LA") and image.getextrema()[-1][0] < 255:
        return image.split()[-1]  # a tip with transparency: its alpha is the ink
    grey = image.convert("L")
    # a dark mark on light paper is the usual tip picture: ink is where it is dark
    return ImageChops.invert(grey) if sum(grey.getdata()) / max(1, grey.width * grey.height) > 127 else grey


def _shape(pattern: str, size: int, b: Brush) -> Image.Image:
    """A stamp's shape (L, ink = 255), pointing along +x (the line's direction)."""
    n = max(3, size)
    tile = Image.new("L", (n * 3 if pattern == "dash" else n, n), 0)
    d = ImageDraw.Draw(tile)
    w, h = tile.size
    if pattern == "dash":
        d.rectangle((0, h * 0.3, w - 1, h * 0.7), fill=255)
    elif pattern == "lace":
        d.arc((0, 0, w - 1, h * 2 - 1), 180, 360, fill=255, width=max(1, n // 8))
        r = max(1, n // 10)
        d.ellipse((w / 2 - r, h * 0.55 - r, w / 2 + r, h * 0.55 + r), fill=255)
    elif pattern == "grass":
        d.polygon([(w * 0.35, h - 1), (w * 0.65, h - 1), (w * 0.55, 0)], fill=255)  # a blade, pointing up
    elif pattern == "leaves":
        d.ellipse((0, h * 0.3, w - 1, h * 0.7), fill=255)
        d.line((w * 0.1, h / 2, w * 0.95, h / 2), fill=90, width=max(1, n // 16))
    elif pattern == "hearts":
        r = w / 4
        d.ellipse((w * 0.05, h * 0.1, w * 0.05 + 2 * r, h * 0.1 + 2 * r), fill=255)
        d.ellipse((w * 0.95 - 2 * r, h * 0.1, w * 0.95, h * 0.1 + 2 * r), fill=255)
        d.polygon([(w * 0.07, h * 0.38), (w * 0.93, h * 0.38), (w / 2, h * 0.95)], fill=255)
    elif pattern == "stars":
        pts = []
        for k in range(10):
            r = (w / 2) * (1.0 if k % 2 == 0 else 0.42)
            a = -math.pi / 2 + k * math.pi / 5
            pts.append((w / 2 + r * math.cos(a), h / 2 + r * math.sin(a)))
        d.polygon(pts, fill=255)
    elif b.tip == "flat":
        thin = max(1.0, h * b.tip_ratio)
        d.ellipse((0, h / 2 - thin / 2, w - 1, h / 2 + thin / 2), fill=255)
    else:  # dots and round stamps
        d.ellipse((0, 0, w - 1, h - 1), fill=255)
    return tile


def _stamped(b: Brush) -> bool:
    return bool(b.pattern) or b.tip in ("flat", "image") or b.scatter > 0 or b.spacing > 0


def _draw_stamps(mask: Image.Image, pts: list, b: Brush, dpi: int, width_mm: float, seed: str) -> None:
    """Lay the brush's stamp along the line (points in px with their radius)."""
    rng = random.Random(seed or "genko")
    width_px = width_mm * dpi / 25.4
    step = max(1.0, (b.spacing or 0.1) * width_px)
    tip = _decode_tip(b.tip_png) if b.tip == "image" and b.tip_png else None
    cache: dict = {}
    # walk the line at `step`, carrying the leftover distance between segments
    carry = 0.0
    positions = []
    for (ax, ay, ar), (bx, by, br) in zip(pts, pts[1:] or pts):
        length = math.hypot(bx - ax, by - ay)
        direction = math.atan2(by - ay, bx - ax) if length > 1e-6 else 0.0
        t = carry
        while t <= length:
            f = t / length if length > 1e-6 else 0.0
            positions.append((ax + (bx - ax) * f, ay + (by - ay) * f, ar + (br - ar) * f, direction))
            t += step
        carry = t - length
        if length <= 1e-6:
            break
    for x, y, r, direction in positions:
        for _ in range(b.count):
            size = max(1.5, 2 * r * b.stamp_size * (1 - b.size_jitter * rng.random()))  # (a spray drop stays visible)
            ox = oy = 0.0
            if b.scatter:
                spread = b.scatter * width_px
                ox, oy = rng.gauss(0, spread / 2), rng.gauss(0, spread / 2)
            if b.pattern == "grass":
                angle = rng.uniform(-0.35, 0.35)  # blades stand up (the shape already points up), a little astray
            elif b.turn_jitter:
                angle = rng.uniform(0, math.tau)
            elif b.tip == "flat" and not b.tip_follow:
                angle = math.radians(b.tip_angle)
            elif b.pattern in ("dash", "lace", "leaves") or b.tip_follow:
                angle = direction
            else:
                angle = 0.0
            key = (round(size), round(math.degrees(angle) / 3))
            stamp = cache.get(key)
            if stamp is None:
                if tip is not None:
                    base = tip.resize((max(1, round(size)), max(1, round(size * tip.height / max(1, tip.width)))))
                else:
                    base = _shape(b.pattern, max(3, round(size)), b)
                stamp = base.rotate(-math.degrees(angle), expand=True, resample=Image.Resampling.BICUBIC) if angle else base
                if len(cache) < 400:
                    cache[key] = stamp
            px, py = round(x + ox - stamp.width / 2), round(y + oy - stamp.height / 2)
            box = (px, py, px + stamp.width, py + stamp.height)
            if box[2] <= 0 or box[3] <= 0 or box[0] >= mask.width or box[1] >= mask.height:
                continue
            mask.paste(ImageChops.lighter(mask.crop(box), stamp), box[:2])


def _finish_edges(mask: Image.Image, b: Brush, dpi: int) -> Image.Image:
    """水彩 edges and the anti-aliasing choice."""
    if b.texture == "water":
        # 水彩境界: the colour gathers at the rim, the middle stays light
        inner = mask.filter(ImageFilter.MinFilter(max(3, (round(dpi / 40) // 2) * 2 + 1)))
        rim = ImageChops.subtract(mask, inner)
        mask = ImageChops.add(mask.point(lambda v: v * 45 // 100), rim.point(lambda v: v * 90 // 100))
    if b.aa == "none":
        mask = mask.point(lambda v: 255 if v >= 128 else 0)
    elif b.aa == "weak":
        mask = mask.point(lambda v: 0 if v < 64 else 255 if v > 192 else (v - 64) * 2)
    elif b.aa == "strong":
        mask = mask.filter(ImageFilter.GaussianBlur(max(0.6, dpi / 300)))
    return mask


def draw(size: tuple[int, int], points: list, dpi: int, width_mm: float, kind: str | None, seed: str = ""):
    """The line's coverage at this resolution, only around the line: (L image, (x0, y0)) or None."""
    b = brush(kind)
    pts = _pressured(points, b)
    if not pts:
        return None
    scale = dpi / 25.4
    reach = 0.75
    if _stamped(b):  # stamps reach further: long dashes, stars, spray thrown off the line
        reach = max(0.75, 0.75 * b.stamp_size * (3 if b.pattern == "dash" else 1.5) + 1.5 * b.scatter)
    pad = width_mm * scale * (1.5 if b.texture == "soft" else reach) + 3
    xs = [p[0] * scale for p in pts]
    ys = [p[1] * scale for p in pts]
    x0, y0 = max(0, int(min(xs) - pad)), max(0, int(min(ys) - pad))
    x1, y1 = min(size[0], int(max(xs) + pad) + 1), min(size[1], int(max(ys) + pad) + 1)
    if x1 <= x0 or y1 <= y0:
        return None
    shift = [(p[0] - x0 / scale, p[1] - y0 / scale, p[2]) for p in pts]
    mask = Image.new("L", (x1 - x0, y1 - y0), 0)
    if _stamped(b):
        radius = [(p[0] * scale, p[1] * scale, max(0.5, width_mm * max(0.03, min(1.5, p[2])) * scale / 2)) for p in shift]
        _draw_stamps(mask, radius, b, dpi, width_mm, seed)
        return _finish_edges(mask, b, dpi), (x0, y0)
    if b.texture == "soft":
        # an airbrush: a wide soft spray (the width is its diameter)
        draw_stroke_mm(ImageDraw.Draw(mask), shift, dpi, width_mm * 0.5, 255)
        return mask.filter(ImageFilter.GaussianBlur(max(1.0, width_mm * scale / 3))), (x0, y0)
    draw_stroke_mm(ImageDraw.Draw(mask), shift, dpi, width_mm, 255, floor=0.03 if b.min_pressure < 0.05 else 0.15)
    if b.texture in ("grain", "dry"):
        rng = random.Random(seed or "genko")
        grain_px = max(1, round(dpi / 150)) if b.texture == "grain" else max(1, round(dpi / 60))
        small = Image.new("L", (max(1, mask.width // grain_px), max(1, mask.height // grain_px)))
        keep = 0.72 if b.texture == "grain" else 0.9
        small.putdata([255 if rng.random() < keep else 70 for _ in range(small.width * small.height)])
        mask = ImageChops.multiply(mask, small.resize(mask.size, Image.Resampling.NEAREST))
    return _finish_edges(mask, b, dpi), (x0, y0)
