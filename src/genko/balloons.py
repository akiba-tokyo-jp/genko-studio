"""Balloons and lettering: shapes, tails, joined balloons, and the text inside them.

A line's box (x_mm, y_mm, w_mm, h_mm) is the balloon's outside. The shape is drawn as a mask with its
tails; balloons that share `style.group` are one mask, so joined balloons have one outline. The
outline is the mask's inner edge (`border_mm` wide) and the inside is filled white (or left empty).
The text is set in the space inside the shape, centred, and shrinks to fit unless it has a size.

Shapes: speech (ellipse), rounded, box, cloud, thought (ellipse and bubbles), shout (spikes),
electric (電子音: a jagged, cornered edge and a lightning tail, for phones and TVs), flash (radiating lines), whisper (dashed), narration (box, no tail), sfx (outlined lettering, no
balloon), none (text only).
"""

from __future__ import annotations

import math
import random

from PIL import Image, ImageChops, ImageDraw, ImageFilter

from genko import fonts
from genko.tategaki import bold_px, cells, compose, draw_mark

SHAPES = ("speech", "rounded", "box", "cloud", "thought", "shout", "electric", "flash", "whisper", "narration", "sfx", "none",
          "picture")
ELLIPTIC = ("speech", "cloud", "thought", "shout", "electric", "flash", "whisper")
NO_TAIL = ("narration", "sfx", "none", "flash", "picture")
SQRT2 = 2 ** 0.5
CAP_MM = 5.0
SFX_CAP_MM = 12.0
OUTLINE = (20, 20, 20)
PAPER = (255, 255, 255)
TEXT = (10, 10, 10)

DEFAULTS = {"font": None, "size_mm": None, "tracking": 0.0, "leading": 0.15, "align": "top", "outline_mm": None,
            "rgb": None, "tcy": True, "border_mm": 0.35, "fill": "white", "group": None, "rotate_deg": 0.0, "skew_deg": 0.0,
            "arc": 0.0, "latin": "rotate", "emphasis_mark": "sesame", "bold": False, "weight": None, "italic": False, "outline_rgb": None,
            "wobble": 0.0, "double": False, "spikes": None, "spike_depth": 0.2,
            # J6: 長体・平体 (width of the letters to their height), a gradient over the letters, the letters set
            # along a path (mm from the box's top left), OpenType features (字形: jp78, jp90, trad, expt, hwid…),
            # 約物の詰め (paired punctuation set half wide), uneven spikes, the cloud's bumps
            "scale_x": 1.0, "gradient": None, "text_path": None, "features": None, "yakumono": True,
            "spike_jitter": 0.0, "bumps": None, "picture": None,
            # the letters' box pulled into four corners (0..1 of it: 遠近・ゆがみ), or painted with a picture
            "warp": None, "fill_png": None}
TAIL_KINDS = ("wedge", "zigzag", "fade", "bubbles")
VS = (range(0xFE00, 0xFE10), range(0xE0100, 0xE01F0))


def is_vs(char: str) -> bool:
    """A variation selector (異体字セレクタ): it chooses the form of the letter before it."""
    return len(char) == 1 and any(ord(char) in r for r in VS)


OPENING = frozenset("「『（(【［〈《〔｛“‘")
CLOSING = frozenset("」』）)】］〉》〕｝”’、。，．・：；")
LINE_START = frozenset("、。，．）」』)】］〉》ーぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮ！？!?…‥")
LINE_END = frozenset("「『（(【［〈《〔")


def px(mm: float, dpi: int) -> int:
    return max(1, round(mm / 25.4 * dpi))


def style_of(line) -> dict:
    return {**DEFAULTS, **(getattr(line, "style", None) or {})}


def tails_of(line) -> list[dict]:
    tails = [dict(t) for t in (getattr(line, "tails", None) or []) if t.get("to")]
    if not tails and getattr(line, "tail", None):
        tails = [{"to": list(line.tail)}]
    return tails


# --- text ------------------------------------------------------------------------------------------------


def _inner(kind: str, w: float, h: float, pad: float) -> tuple[float, float]:
    """The space for text inside the shape."""
    if kind == "electric":  # (a squarish outline: more room than an ellipse, less the teeth)
        return (w - 2 * pad) * 0.76, (h - 2 * pad) * 0.76
    if kind in ELLIPTIC:
        return (w - 2 * pad) / SQRT2, (h - 2 * pad) / SQRT2
    if kind == "rounded":
        r = min(w, h) * 0.3
        return w - 2 * pad - r * 0.3, h - 2 * pad - r * 0.3
    if kind in ("box", "narration"):
        return w - 2 * pad, h - 2 * pad
    if kind == "picture":  # (a picture balloon: the words keep to its middle)
        return (w - 2 * pad) * 0.7, (h - 2 * pad) * 0.7
    return w, h


def _emphasis(line, face) -> list[str]:
    return [face.normalize(str(run)) for run in getattr(line, "emphasis_runs", None) or [] if run]


def _vertical(line, st: dict, face, em: int, inner_h: float, fill) -> Image.Image:
    text = face.normalize(line.text or "")
    tracking, leading = float(st["tracking"] or 0), float(st["leading"] or 0)
    latin = st["latin"] != "upright"
    step = em * (1 + tracking)
    column = inner_h
    if "\n" not in text and em > 0:
        # balance the columns (7 / 7 / 1 reads badly; 5 / 5 / 5 does not)
        count = len(cells(text, bool(st["tcy"]), latin))
        fit = max(1, int((inner_h - em) // step) + 1)
        if count > fit:
            cols = -(-count // fit)
            column = min(column, em + step * (-(-count // cols) - 1))
    return compose(text, face.font(em), em, max(em, int(column + em * 0.1)), fill=fill,
                   ruby_runs=getattr(line, "ruby_runs", None) or None, face=face, tracking=tracking, leading=leading,
                   tcy=bool(st["tcy"]), align=str(st["align"] or "top"), latin=latin,
                   emphasis_runs=_emphasis(line, face) or None, emphasis_mark=str(st["emphasis_mark"] or "sesame"),
                   style_runs=getattr(line, "style_runs", None) or None, bold=line_weight(st))


def _horizontal(line, st: dict, face, em: int, inner_w: float, fill) -> Image.Image:
    """Rows left to right with kinsoku, centred (or aligned by style.align: left / center / right).
    Ruby sits above its words and 傍点 just above the characters (the ruby above them); part of a line
    can be larger, smaller, bolder or in another colour (style_runs), which makes its row taller."""
    from genko.tategaki import bold_px, char_styles

    text = face.normalize(line.text or "")
    tracking = float(st["tracking"] or 0)
    flat = text.replace("\n", "")
    styles_flat = char_styles(flat, getattr(line, "style_runs", None), {"bold": line_weight(st)} if line_weight(st) else None)
    styles: list[dict] = []
    k = 0
    for char in text:  # (styles per character of the text, "\n" included)
        if char == "\n":
            styles.append({})
        else:
            styles.append(styles_flat[k])
            k += 1
    marked: set[int] = set()
    pos = 0
    for base in _emphasis(line, face):
        at = text.find(base, pos)
        if at >= 0:
            marked.update(i for i in range(at, at + len(base)) if not text[i].isspace())
            pos = at + len(base)
    ruby_at: list[tuple[int, int, str]] = []  # (first char, last char + 1, ruby)
    pos = 0
    for run in getattr(line, "ruby_runs", None) or []:
        if not run or len(run) < 2 or not run[0] or not run[1]:
            continue
        base = face.normalize(str(run[0]))
        at = text.find(base, pos)
        if at >= 0:
            ruby_at.append((at, at + len(base), str(run[1])))
            pos = at + len(base)
    mark_h = round(em * 0.36) if marked else 0
    ruby_h = max(6, em // 2) if ruby_at else 0

    def size_of(i: int) -> int:
        return max(4, round(em * max(0.3, min(3.0, float(styles[i].get("scale", 1.0))))))

    features = [str(f) for f in st.get("features") or []] or None

    def advance(i: int) -> float:
        char = text[i]
        if is_vs(char):
            return 0.0
        try:
            width = face.font(size_of(i), char).getlength(char) + em * tracking
        except Exception:
            width = size_of(i) * (1 + tracking)
        if st.get("yakumono", True) and i + 1 < len(text):
            after = text[i + 1]
            # 約物の詰め: a closing mark before another mark, or any mark before an opening one, keeps half its width
            if (char in CLOSING and (after in CLOSING or after in OPENING)) or (char in OPENING and after in OPENING):
                width -= size_of(i) * 0.5
        return width

    rows: list[list[int]] = []  # the characters' places in the text, row by row
    index = 0
    for part in text.split("\n"):
        row: list[int] = []
        width = 0.0
        for char in part:
            w = advance(index)
            if row and width + w > inner_w and char not in LINE_START:
                if text[row[-1]] in LINE_END and len(row) > 1:
                    rows.append(row[:-1])
                    row, width = [row[-1]], advance(row[-1])
                else:
                    rows.append(row)
                    row, width = [], 0.0
            row.append(index)
            index += 1
            width += w
        rows.append(row)
        index += 1  # the "\n"
    above = mark_h + ruby_h
    heights = [round(max([size_of(i) for i in row] or [em]) * (1.15 + float(st["leading"] or 0))) + above for row in rows]
    widths = [sum(advance(i) for i in row) for row in rows]
    out_w = max(1, math.ceil(max(widths or [1])))
    out = Image.new("RGBA", (out_w, max(1, sum(heights))), (0, 0, 0, 0))
    draw = ImageDraw.Draw(out)
    align = st["align"] if st["align"] in ("left", "right") else "center"
    xs: dict[int, tuple[float, float, int]] = {}  # place in text → (left, right, row top)
    top = 0
    for r, row in enumerate(rows):
        x = 0.0 if align == "left" else (out_w - widths[r]) / (2 if align == "center" else 1)
        tallest = max([size_of(i) for i in row] or [em])
        base_y = top + above + (heights[r] - above - tallest) / 2  # characters share the row's baseline area
        for i in row:
            size = size_of(i)
            rgb = tuple(styles[i].get("rgb") or fill)
            thick = bold_px(size, styles[i].get("bold"))
            if is_vs(text[i]):
                xs[i] = (x, x, top)
                continue
            letter = text[i] + "".join(c for c in text[i + 1:i + 3] if is_vs(c))[:1]
            draw.text((x, base_y + (tallest - size)), letter, font=face.font(size, text[i]), fill=rgb + (255,),
                      stroke_width=thick, stroke_fill=rgb + (255,) if thick else None, features=features)
            w = advance(i) - em * tracking
            if i in marked:
                draw_mark(out, (x + w / 2, top + ruby_h + mark_h / 2 + max(1, em // 16)), mark_h, str(st["emphasis_mark"] or "sesame"),
                          rgb, vertical=False)
            xs[i] = (x, x + w, top)
            x += advance(i)
        top += heights[r]
    for first, last, ruby in ruby_at:
        # a word split over rows gets its ruby split in proportion
        groups: dict[int, list[int]] = {}
        for i in range(first, last):
            if i in xs:
                groups.setdefault(xs[i][2], []).append(i)
        taken = 0
        count = sum(len(g) for g in groups.values()) or 1
        for n, (row_top, chars) in enumerate(sorted(groups.items())):
            share = len(ruby) - taken if n == len(groups) - 1 else round(len(ruby) * len(chars) / count)
            part = ruby[taken:taken + share]
            taken += share
            if not part:
                continue
            font = face.font(ruby_h, part[0])
            span = font.getlength(part)
            centre = (xs[chars[0]][0] + xs[chars[-1]][1]) / 2
            draw.text((max(0, min(out_w - span, centre - span / 2)), row_top), part, font=font, fill=fill + (255,))
    return out


def text_image(line, dpi: int, font_path: str | None = None) -> tuple[Image.Image, int]:
    """The lettering of a line (RGBA) and its em in px, fitted into its balloon."""
    st = style_of(line)
    kind = line.balloon or "speech"
    face = fonts.face(st["font"] or (None if kind == "sfx" else font_path), fonts.DEFAULT_SFX if kind == "sfx" else fonts.DEFAULT_DIALOGUE)
    fill = tuple(st["rgb"]) if st["rgb"] else TEXT
    w, h = px(line.w_mm or 40, dpi), px(line.h_mm or 20, dpi)
    vertical = getattr(line, "wrap", "horizontal") == "vertical"
    if st["size_mm"]:
        em = px(float(st["size_mm"]), dpi)
        fixed = True
    elif kind == "sfx":
        first = max(1, len(cells((line.text or " ").split("\n")[0], bool(st["tcy"]), st["latin"] != "upright")))
        em = max(8, min(px(SFX_CAP_MM, dpi), (w if vertical else h), (h if vertical else w) // first))
        fixed = False
    elif kind == "none":
        # text only: the box sets the size (the longest column fills its height)
        longest = max(len(cells(part, bool(st["tcy"]))) for part in (line.text or " ").split("\n")) or 1
        em = max(8, min(w, h // longest) if vertical else min(h, w // longest))
        fixed = False
    else:
        em = max(8, px(CAP_MM, dpi))
        fixed = False
    pad = 0 if kind in ("none", "sfx") else max(2, em // 4)
    inner_w, inner_h = _inner(kind, w, h, pad)
    scale_x = max(0.3, min(3.0, float(st.get("scale_x") or 1.0)))
    for _ in range(10):
        image = _vertical(line, st, face, em, inner_h, fill) if vertical else _horizontal(line, st, face, em, inner_w, fill)
        if abs(scale_x - 1) > 1e-3:  # 長体 (< 1) or 平体 (> 1): the letters narrower or wider than tall
            image = image.resize((max(1, round(image.width * scale_x)), image.height), Image.Resampling.LANCZOS)
        if fixed or (image.width <= inner_w * 1.02 and image.height <= inner_h * 1.02) or em <= 8:
            break
        em = max(8, int(em * 0.9))
        pad = 0 if kind in ("none", "sfx") else max(2, em // 4)
        inner_w, inner_h = _inner(kind, w, h, pad)
    if st.get("gradient"):
        image = gradient_letters(image, st["gradient"])
    if st.get("fill_png"):
        image = picture_letters(image, st["fill_png"])
    outline = st["outline_mm"]
    grow = px(float(outline), dpi) if outline else (max(2, em // 8) if kind == "sfx" else 0)
    if grow:
        image = outlined(image, grow, tuple(st["outline_rgb"]) if st["outline_rgb"] else (255, 255, 255))
    if st["arc"]:
        image = arched(image, float(st["arc"]), vertical)
    skew = float(st["skew_deg"] or 0) or (12.0 if st["italic"] else 0.0)  # (italic: a light lean)
    if skew:
        image = skewed(image, skew, vertical)
    if st.get("warp"):
        image = warped_letters(image, st["warp"])
    return image, em


_PICTURES: dict = {}


def picture_of(data: str) -> Image.Image | None:
    """A picture balloon's image (base64 PNG), decoded once."""
    import base64
    import io

    key = hash(data)
    if key not in _PICTURES:
        try:
            _PICTURES[key] = Image.open(io.BytesIO(base64.b64decode(data))).convert("RGBA")
        except Exception:
            _PICTURES[key] = None
        if len(_PICTURES) > 64:
            _PICTURES.pop(next(iter(_PICTURES)))
    return _PICTURES[key]


def picture_letters(image: Image.Image, data: str) -> Image.Image:
    """The letters painted with a picture (stretched over them)."""
    picture = picture_of(data)
    if picture is None:
        return image
    box = image.getchannel(3).getbbox() or (0, 0, image.width, image.height)
    out = Image.new("RGBA", image.size, (0, 0, 0, 0))
    out.paste(picture.convert("RGB").resize((max(1, box[2] - box[0]), max(1, box[3] - box[1])), Image.Resampling.LANCZOS), box[:2])
    out.putalpha(image.getchannel(3))
    return out


def warped_letters(image: Image.Image, corners) -> Image.Image:
    """文字の変形: the letters' box pulled so its top-left, top-right, bottom-right and bottom-left corners go to
    these places (each [x, y] as shares of the box; [[0,0],[1,0],[1,1],[0,1]] leaves it as it is)."""
    from genko.warp import _homography

    w, h = image.size
    dst = [(float(c[0]) * w, float(c[1]) * h) for c in corners]
    xs, ys = [p[0] for p in dst], [p[1] for p in dst]
    ox, oy = min(xs), min(ys)
    size = (max(1, int(max(xs) - ox) + 1), max(1, int(max(ys) - oy) + 1))
    src = [(0, 0), (w, 0), (w, h), (0, h)]
    back = _homography([(x - ox, y - oy) for x, y in dst], src)  # output place → where it comes from
    coeffs = tuple((back / back[2, 2]).flatten()[:8])
    return image.transform(size, Image.Transform.PERSPECTIVE, coeffs, Image.Resampling.BICUBIC)


def gradient_letters(image: Image.Image, spec: dict) -> Image.Image:
    """The letters coloured from one colour to another (top to bottom, or at `angle` degrees)."""
    import numpy as np

    w, h = image.size
    a = np.array([int(v) for v in (spec.get("rgb_from") or [20, 20, 20])][:3], dtype=float)
    b = np.array([int(v) for v in (spec.get("rgb_to") or [230, 40, 40])][:3], dtype=float)
    angle = math.radians(float(spec.get("angle", 90)))
    x0, y0, x1, y1 = image.getchannel(3).getbbox() or (0, 0, w, h)  # (from the letters' own edges)
    gx, gy = np.meshgrid((np.arange(w) - x0) / max(1, x1 - x0 - 1), (np.arange(h) - y0) / max(1, y1 - y0 - 1))
    t = np.clip(gx * math.cos(angle) + gy * math.sin(angle), 0, None)
    top = abs(math.cos(angle)) + abs(math.sin(angle))
    t = np.clip(t / max(1e-6, top), 0, 1)
    rgb = a[None, None, :] * (1 - t[..., None]) + b[None, None, :] * t[..., None]
    out = Image.fromarray(rgb.round().astype("uint8"), "RGB").convert("RGBA")
    out.putalpha(image.split()[3])
    return out


def path_text(image: Image.Image, line, dpi: int, font_path: str | None) -> None:
    """文字をパスに沿わせる: each letter stood on the path (mm from the line's box), turned with it."""
    st = style_of(line)
    face = fonts.face(st["font"] or font_path, fonts.DEFAULT_SFX if (line.balloon or "") == "sfx" else fonts.DEFAULT_DIALOGUE)
    fill = tuple(st["rgb"]) if st["rgb"] else TEXT
    em = px(float(st["size_mm"] or CAP_MM), dpi)
    pts = [(px(line.x_mm + float(p[0]), dpi), px(line.y_mm + float(p[1]), dpi)) for p in st["text_path"]]
    if len(pts) < 2:
        return
    lengths = [0.0]
    for a, b in zip(pts, pts[1:]):
        lengths.append(lengths[-1] + math.dist(a, b))
    total = lengths[-1]
    text = face.normalize((line.text or "").replace("\n", ""))
    letters = [c for c in text if c != "\n" and not is_vs(c)]
    tracking = float(st["tracking"] or 0)
    widths = [face.font(em, c).getlength(c) * float(st.get("scale_x") or 1) + em * tracking for c in letters]
    run = sum(widths)
    at = {"left": 0.0, "right": max(0.0, total - run)}.get(st["align"], max(0.0, (total - run) / 2))
    bold = bold_px(em, line_weight(st)) if line_weight(st) else 0

    def point_at(d: float):
        d = max(0.0, min(total, d))
        k = max(0, min(len(pts) - 2, next((i for i in range(len(lengths) - 1) if lengths[i + 1] >= d), len(pts) - 2)))
        seg = (lengths[k + 1] - lengths[k]) or 1.0
        t = (d - lengths[k]) / seg
        (x0, y0), (x1, y1) = pts[k], pts[k + 1]
        return x0 + (x1 - x0) * t, y0 + (y1 - y0) * t, math.degrees(math.atan2(y1 - y0, x1 - x0))

    for char, width in zip(letters, widths):
        x, y, angle = point_at(at + width / 2)
        at += width
        if char.isspace():
            continue
        font = face.font(em, char)
        size = em * 3
        cell = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        ImageDraw.Draw(cell).text((size / 2, size / 2), char, font=font, fill=fill + (255,), anchor="ms",
                                  stroke_width=bold, stroke_fill=fill + (255,) if bold else None)
        if abs(float(st.get("scale_x") or 1) - 1) > 1e-3:
            cell = cell.resize((max(1, round(size * float(st["scale_x"]))), size), Image.Resampling.LANCZOS)
        if st.get("gradient"):
            cell = gradient_letters(cell, st["gradient"])
        if st["outline_mm"]:
            cell = outlined(cell, px(float(st["outline_mm"]), dpi), tuple(st["outline_rgb"]) if st["outline_rgb"] else (255, 255, 255))
        turned = cell.rotate(-angle, resample=Image.Resampling.BICUBIC, center=(cell.width / 2, cell.height / 2))
        image.paste(turned, (round(x - turned.width / 2), round(y - turned.height / 2)), turned)


def skewed(image: Image.Image, degrees: float, vertical: bool) -> Image.Image:
    """Lettering leaning by `degrees`: across text leans right like italics (its top moves right);
    vertical text's columns slide down toward the left (a slanted 描き文字). Negative leans the other way."""
    k = math.tan(math.radians(max(-60.0, min(60.0, degrees))))
    w, h = image.size
    # (PIL's affine maps each output pixel back to the input: x' = a x + b y + c, y' = d x + e y + f)
    if vertical:
        size = (w, int(h + abs(k) * w) + 1)
        return image.transform(size, Image.Transform.AFFINE, (1, 0, 0, k, 1, -k * w if k > 0 else 0), Image.Resampling.BICUBIC)
    size = (int(w + abs(k) * h) + 1, h)
    return image.transform(size, Image.Transform.AFFINE, (1, k, -k * h if k > 0 else 0, 0, 1, 0), Image.Resampling.BICUBIC)


def arched(image: Image.Image, amount: float, vertical: bool) -> Image.Image:
    """Lettering bent into a bow (弓なり): amount 1 lifts the middle by a third of the text's length
    (negative bends the other way)."""
    w, h = image.size
    length = h if vertical else w
    rise = abs(amount) * length / 3
    pad = int(rise) + 1
    out = Image.new("RGBA", (w + pad, h) if vertical else (w, h + pad), (0, 0, 0, 0))
    strip = max(1, length // 120)
    for start in range(0, length, strip):
        t = (start + strip / 2) / length * 2 - 1  # -1 … 1 along the text
        lift = rise * (1 - t * t)  # the middle moves most
        offset = int(round(lift if amount < 0 else rise - lift))
        if vertical:
            piece = image.crop((0, start, w, min(h, start + strip)))
            out.alpha_composite(piece, (pad - offset if amount > 0 else offset, start))
        else:
            piece = image.crop((start, 0, min(w, start + strip), h))
            out.alpha_composite(piece, (start, offset))
    return out


def outlined(text_img: Image.Image, grow: int, colour=(255, 255, 255)) -> Image.Image:
    """Text with a halo (白フチ) `grow` px wide."""
    alpha = text_img.split()[3]
    pad = grow + 1
    padded = Image.new("L", (alpha.width + 2 * pad, alpha.height + 2 * pad), 0)
    padded.paste(alpha, (pad, pad))
    halo = padded.filter(ImageFilter.MaxFilter(min(grow * 2 + 1, 61) | 1))
    out = Image.new("RGBA", padded.size, colour + (0,))
    out.putalpha(halo)
    body = Image.new("RGBA", padded.size, (0, 0, 0, 0))
    body.paste(text_img, (pad, pad))
    out.alpha_composite(body)
    return out


# --- shapes -----------------------------------------------------------------------------------------------


def line_weight(st: dict) -> int:
    """The line's weight: style.weight (normal / bold / heavy) wins over the older bold switch."""
    from genko.tategaki import weight_level

    if st.get("weight"):
        return weight_level(st["weight"])
    return weight_level(bool(st.get("bold")))


def _ellipse_point(cx: float, cy: float, rx: float, ry: float, t: float) -> tuple[float, float]:
    return cx + rx * math.cos(t), cy + ry * math.sin(t)


def _wobbly(points: list, amount: float, size: float, seed: str) -> list:
    """An outline pushed in and out a little, smoothly (a balloon drawn by hand)."""
    import random

    rng = random.Random(seed or "genko")
    waves = [(rng.uniform(2, 5), rng.uniform(0, math.tau), rng.uniform(0.5, 1.0)) for _ in range(3)]
    cx = sum(p[0] for p in points) / len(points)
    cy = sum(p[1] for p in points) / len(points)
    out = []
    n = len(points)
    for k, (x, y) in enumerate(points):
        t = math.tau * k / n
        push = sum(a * math.sin(f * t + ph) for f, ph, a in waves) / 3
        d = math.hypot(x - cx, y - cy) or 1.0
        shift = push * amount * size * 0.05
        out.append((x + (x - cx) / d * shift, y + (y - cy) / d * shift))
    return out


def _outline(kind: str, box, n: int = 96) -> list | None:
    """The outline of an ellipse or box balloon as points (for the hand-drawn wobble)."""
    x0, y0, x1, y1 = box
    cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    if kind in ("speech", "thought", "whisper", "flash"):
        return [_ellipse_point(cx, cy, rx, ry, math.tau * k / n) for k in range(n)]
    if kind in ("box", "narration", "rounded"):
        per = n // 4
        pts = []
        for (ax, ay), (bx, by) in (((x0, y0), (x1, y0)), ((x1, y0), (x1, y1)), ((x1, y1), (x0, y1)), ((x0, y1), (x0, y0))):
            pts += [(ax + (bx - ax) * i / per, ay + (by - ay) * i / per) for i in range(per)]
        return pts
    return None


def _shape(draw: ImageDraw.ImageDraw, kind: str, box: tuple[float, float, float, float], st: dict | None = None,
           seed: str = "") -> None:
    x0, y0, x1, y1 = box
    cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    st = st or {}
    wobble = float(st.get("wobble") or 0)
    if wobble > 0:
        points = _outline(kind, box)
        if points:
            draw.polygon(_wobbly(points, wobble, min(rx, ry) * 2, seed), fill=255)
            return
    if kind in ("box", "narration"):
        draw.rectangle(box, fill=255)
    elif kind == "rounded":
        draw.rounded_rectangle(box, radius=min(rx, ry) * 0.6, fill=255)
    elif kind == "cloud":
        bump = max(2.0, min(rx, ry) * 0.3)
        k = 1 - bump / max(1.0, min(rx, ry))
        perimeter = math.pi * (rx + ry) * k
        n = int(st.get("bumps") or 0) or max(8, int(perimeter / (bump * 1.4)))
        if st.get("bumps"):  # (a set number of bumps: each as big as its share of the edge)
            bump = max(2.0, perimeter / n / 1.4)
        draw.ellipse((cx - rx * k, cy - ry * k, cx + rx * k, cy + ry * k), fill=255)
        for i in range(n):
            bx, by = _ellipse_point(cx, cy, rx * k, ry * k, 2 * math.pi * i / n)
            draw.ellipse((bx - bump, by - bump, bx + bump, by + bump), fill=255)
    elif kind == "shout":
        spikes = int(st.get("spikes") or 0) or max(12, int((rx + ry) / max(4.0, min(rx, ry) / 3)))
        depth = max(0.05, min(0.6, float(st.get("spike_depth") or 0.2)))
        points = []
        jitter = max(0.0, min(1.0, float(st.get("spike_jitter") or 0)))
        rng = random.Random(seed or "shout")
        for i in range(spikes * 2):
            t = math.pi * i / spikes
            k = (1.0 + (rng.uniform(-0.5, 0.5) * depth * 2 * jitter)) if i % 2 == 0 else 1.0 - depth
            points.append(_ellipse_point(cx, cy, rx * k, ry * k, t))
        draw.polygon(points, fill=255)
    elif kind == "electric":
        draw.polygon(_electric(box, st), fill=255)
    else:  # speech, thought, whisper, flash
        draw.ellipse(box, fill=255)


def _electric(box, st: dict | None = None) -> list[tuple[float, float]]:
    """The 電子音 edge: a squarish outline (a superellipse) broken into sharp zigzags that lean one way,
    like a voice coming through a speaker."""
    x0, y0, x1, y1 = box
    cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    st = st or {}
    teeth = int(st.get("spikes") or 0) or max(14, int((rx + ry) / max(3.0, min(rx, ry) / 4)))
    depth = max(0.04, min(0.4, float(st.get("spike_depth") or 0.12)))
    points = []
    for i in range(teeth * 2):
        t = math.pi * (i + (0.35 if i % 2 else 0)) / teeth  # (the inner corners lag: a sawtooth, not a star)
        c, s_ = math.cos(t), math.sin(t)
        # a superellipse of power 4: flat sides, round corners
        k = 1.0 if i % 2 == 0 else 1.0 - depth
        px_ = cx + rx * k * math.copysign(abs(c) ** 0.5, c)
        py_ = cy + ry * k * math.copysign(abs(s_) ** 0.5, s_)
        points.append((px_, py_))
    return points


def _edge_point(kind: str, box, toward: tuple[float, float], spread: float) -> tuple[tuple[float, float], tuple[float, float]]:
    """Two points on the shape's edge, `spread` apart, facing `toward` (the tail's base)."""
    x0, y0, x1, y1 = box
    cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
    tx, ty = toward
    if kind in ("box", "narration", "rounded"):
        if abs(tx - cx) / max(rx, 1) > abs(ty - cy) / max(ry, 1):
            ex = cx + (rx if tx > cx else -rx) * 0.9
            my = max(y0 + spread, min(y1 - spread, ty))
            return (ex, my - spread / 2), (ex, my + spread / 2)
        ey = cy + (ry if ty > cy else -ry) * 0.9
        mx = max(x0 + spread, min(x1 - spread, tx))
        return (mx - spread / 2, ey), (mx + spread / 2, ey)
    t = math.atan2((ty - cy) / max(ry, 1), (tx - cx) / max(rx, 1))
    k = 0.72 if kind == "shout" else 0.8 if kind == "electric" else 0.9  # start inside (below the spikes' valleys)
    rx, ry = rx * k / 0.9, ry * k / 0.9
    lo, hi = 0.0, math.pi / 2
    for _ in range(24):  # the half-angle whose chord is `spread`
        mid = (lo + hi) / 2
        a = _ellipse_point(cx, cy, rx * 0.9, ry * 0.9, t - mid)
        b = _ellipse_point(cx, cy, rx * 0.9, ry * 0.9, t + mid)
        if math.dist(a, b) < spread:
            lo = mid
        else:
            hi = mid
    return _ellipse_point(cx, cy, rx * 0.9, ry * 0.9, t - lo), _ellipse_point(cx, cy, rx * 0.9, ry * 0.9, t + lo)


def _tail_polygon(kind: str, box, tip, via, base: float, style: str = "wedge") -> list[tuple[float, float]]:
    """A tail from the shape toward `tip`, curving through `via` when given (a quadratic curve).
    style: wedge (くさび), zigzag (ギザギザ: the edges saw back and forth), fade (消える: drawn the same, then
    faded out toward the tip where it is painted)."""
    p1, p2 = _edge_point(kind, box, via or tip, base)
    mx, my = (p1[0] + p2[0]) / 2, (p1[1] + p2[1]) / 2
    cx, cy = via if via else ((mx + tip[0]) / 2, (my + tip[1]) / 2)
    left, right = [], []
    steps = 16
    for i in range(steps + 1):
        t = i / steps
        x = (1 - t) ** 2 * mx + 2 * (1 - t) * t * cx + t ** 2 * tip[0]
        y = (1 - t) ** 2 * my + 2 * (1 - t) * t * cy + t ** 2 * tip[1]
        dx = 2 * (1 - t) * (cx - mx) + 2 * t * (tip[0] - cx)
        dy = 2 * (1 - t) * (cy - my) + 2 * t * (tip[1] - cy)
        n = math.hypot(dx, dy) or 1.0
        half = math.dist(p1, p2) / 2 * (1 - t)
        if style == "zigzag" and 0 < i < steps:
            half *= 1.6 if i % 2 else 0.55
        if kind == "electric" and 0 < i < steps:
            # a lightning tail: the centre line jumps sideways at a few steps
            jump = (1 if (i // 4) % 2 else -1) * math.dist(p1, p2) * 0.9 * (1 - t) if i % 4 == 0 else 0.0
            x, y = x - dy / n * jump, y + dx / n * jump
        left.append((x - dy / n * half, y + dx / n * half))
        right.append((x + dy / n * half, y - dx / n * half))
    return left + right[::-1]


def _fade_mask(size, fades) -> Image.Image:
    """255 everywhere but near each fading tail's tip, where it falls to 0 at the tip itself."""
    import numpy as np

    w, h = size
    keep = np.ones((h, w), dtype=np.float32)
    gx, gy = np.meshgrid(np.arange(w), np.arange(h))
    for box, tip in fades:
        cx, cy = (box[0] + box[2]) / 2, (box[1] + box[3]) / 2
        reach = max(1.0, math.dist((cx, cy), tip) - min(box[2] - box[0], box[3] - box[1]) / 2) * 0.7
        d = np.hypot(gx - tip[0], gy - tip[1])
        keep = np.minimum(keep, np.clip(d / reach, 0, 1))
    return Image.fromarray((keep * 255).astype("uint8"), "L")


def _erode(mask: Image.Image, amount: int) -> Image.Image:
    if amount <= 0:
        return mask
    if amount <= 3:
        for _ in range(amount):
            mask = mask.filter(ImageFilter.MinFilter(3))
        return mask
    # an isotropic erosion: blur and keep what stays nearly solid (Φ(2) ≈ 0.977)
    return mask.filter(ImageFilter.GaussianBlur(amount / 2)).point(lambda v: 255 if v >= 249 else 0)


def _turned(point, centre, degrees: float) -> list[float]:
    """A point turned about `centre` by `degrees` (clockwise on the page)."""
    a = math.radians(degrees)
    dx, dy = point[0] - centre[0], point[1] - centre[1]
    return [centre[0] + dx * math.cos(a) - dy * math.sin(a), centre[1] + dx * math.sin(a) + dy * math.cos(a)]


def _draw_turned(image: Image.Image, lines: list, dpi: int, show_speaker: bool, font_path: str | None, degrees: float) -> None:
    """A balloon turned by `degrees`: drawn upright on its own sheet, turned about its centre and laid
    on the page. The tails are turned back first, so they still point where they were aimed."""
    import copy

    x0 = min(ln.x_mm for ln in lines)
    y0 = min(ln.y_mm for ln in lines)
    x1 = max(ln.x_mm + (ln.w_mm or 40) for ln in lines)
    y1 = max(ln.y_mm + (ln.h_mm or 20) for ln in lines)
    centre = ((x0 + x1) / 2, (y0 + y1) / 2)
    reach = math.hypot(x1 - x0, y1 - y0) / 2
    for ln in lines:
        for tail in tails_of(ln):
            reach = max(reach, math.dist(centre, tail["to"]))
    reach += 6  # room for outlines, halos and the tail's bubbles
    left, top = centre[0] - reach, centre[1] - reach
    size = px(2 * reach, dpi)
    sheet = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    upright = []
    for ln in lines:
        twin = copy.copy(ln)
        twin.style = {k: v for k, v in (ln.style or {}).items() if k != "rotate_deg"}
        twin.x_mm, twin.y_mm = ln.x_mm - left, ln.y_mm - top
        twin.path = [(p[0] - left, p[1] - top) for p in ln.path] if getattr(ln, "path", None) else None
        tails = []
        for tail in tails_of(ln):
            moved = dict(tail, to=[v - o for v, o in zip(_turned(tail["to"], centre, -degrees), (left, top))])
            if tail.get("via"):
                moved["via"] = [v - o for v, o in zip(_turned(tail["via"], centre, -degrees), (left, top))]
            tails.append(moved)
        twin.tails = tails
        twin.tail = tuple(tails[0]["to"]) if tails else None
        upright.append(twin)
    draw_group(sheet, upright, dpi, False, font_path)
    turned = sheet.rotate(-degrees, resample=Image.Resampling.BICUBIC, center=(size / 2, size / 2))
    image.paste(turned, (round(left / 25.4 * dpi), round(top / 25.4 * dpi)), turned)
    if show_speaker:
        for ln in lines:
            if ln.speaker and (ln.balloon or "speech") != "none":
                font = fonts.face(None).font(max(8, px(3, dpi)))
                ImageDraw.Draw(image).text((px(ln.x_mm, dpi), max(0, px(ln.y_mm - 4, dpi))), ln.speaker, fill=(90, 90, 90), font=font)


def draw_group(image: Image.Image, lines: list, dpi: int, show_speaker: bool = True, font_path: str | None = None) -> None:
    """One balloon (or several joined ones) with their tails and text, onto the page image."""
    first = lines[0]
    kind = first.balloon or "speech"
    st = style_of(first)
    if st["rotate_deg"] and abs(float(st["rotate_deg"])) > 0.01:
        _draw_turned(image, lines, dpi, show_speaker, font_path, float(st["rotate_deg"]))
        return
    for ln in lines:  # 画像のフキダシ: the picture stretched over the box
        if (ln.balloon or "") == "picture" and style_of(ln).get("picture"):
            picture = picture_of(style_of(ln)["picture"])
            if picture is not None:
                box = (px(ln.x_mm, dpi), px(ln.y_mm, dpi), px(ln.w_mm or 40, dpi), px(ln.h_mm or 20, dpi))
                stretched = picture.resize((max(1, box[2]), max(1, box[3])), Image.Resampling.LANCZOS)
                image.paste(stretched, (box[0], box[1]), stretched)
    if kind == "picture":
        for line in lines:
            _paint_text(image, line, dpi, show_speaker, font_path)
        return
    if kind not in ("sfx", "none"):
        boxes = [(px(ln.x_mm, dpi), px(ln.y_mm, dpi), px(ln.x_mm + (ln.w_mm or 40), dpi), px(ln.y_mm + (ln.h_mm or 20), dpi))
                 for ln in lines]
        tails = [(ln, t) for ln in lines if (ln.balloon or "speech") not in NO_TAIL for t in tails_of(ln)]
        for ln in lines:  # a thought without a speaker still trails its bubbles, down and away
            if (ln.balloon or "speech") == "thought" and not tails_of(ln):
                tails.append((ln, {"to": [ln.x_mm - (ln.w_mm or 40) * 0.3, ln.y_mm + (ln.h_mm or 20) * 1.2]}))
        xs = [b[0] for b in boxes] + [b[2] for b in boxes] + [px(t["to"][0], dpi) for _, t in tails]
        ys = [b[1] for b in boxes] + [b[3] for b in boxes] + [px(t["to"][1], dpi) for _, t in tails]
        margin = px(4, dpi)
        rx0, ry0 = max(0, min(xs) - margin), max(0, min(ys) - margin)
        rx1, ry1 = min(image.width, max(xs) + margin), min(image.height, max(ys) + margin)
        if rx1 > rx0 and ry1 > ry0:
            _paint_shapes(image, lines, boxes, tails, (rx0, ry0, rx1, ry1), dpi, st)
    for line in lines:
        _paint_text(image, line, dpi, show_speaker, font_path)


def _paint_shapes(image, lines, boxes, tails, region, dpi: int, st: dict) -> None:
    rx0, ry0, rx1, ry1 = region
    scale = 2 if dpi < 300 else 1  # draw small renders at twice the size for smooth edges
    size = ((rx1 - rx0) * scale, (ry1 - ry0) * scale)
    mask = Image.new("L", size, 0)
    draw = ImageDraw.Draw(mask)
    bubbles = Image.new("L", size, 0)
    bdraw = ImageDraw.Draw(bubbles)
    fades: list = []

    def local(box):
        return tuple((v - o) * scale for v, o in zip(box, (rx0, ry0, rx0, ry0)))

    for line, box in zip(lines, boxes):
        if getattr(line, "path", None):  # drawn by hand
            draw.polygon([((px(p[0], dpi) - rx0) * scale, (px(p[1], dpi) - ry0) * scale) for p in line.path], fill=255)
        else:
            _shape(draw, line.balloon or "speech", local(box), style_of(line), str(getattr(line, "id", "")))
    for line, tail in tails:
        box = local(next(b for ln, b in zip(lines, boxes) if ln is line))
        tip = ((px(tail["to"][0], dpi) - rx0) * scale, (px(tail["to"][1], dpi) - ry0) * scale)
        via = ((px(tail["via"][0], dpi) - rx0) * scale, (px(tail["via"][1], dpi) - ry0) * scale) if tail.get("via") else None
        short = min(box[2] - box[0], box[3] - box[1])
        base = px(float(tail["width_mm"]), dpi) * scale if tail.get("width_mm") else max(4.0, short / 4)
        tail_style = str(tail.get("kind") or "wedge")
        if (line.balloon or "speech") == "thought" or tail_style == "bubbles":
            # bubbles toward the speaker instead of a tail
            x0, y0, x1, y1 = box
            cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
            ang = math.atan2((tip[1] - cy) / max(ry, 1), (tip[0] - cx) / max(rx, 1))
            ex, ey = _ellipse_point(cx, cy, rx, ry, ang)
            for i, frac in enumerate((0.3, 0.6, 0.88)):
                r = max(2.0, min(rx, ry) * (0.2 - i * 0.05))
                bx, by = ex + (tip[0] - ex) * frac, ey + (tip[1] - ey) * frac
                bdraw.ellipse((bx - r, by - r, bx + r, by + r), fill=255)
            continue
        draw.polygon(_tail_polygon(line.balloon or "speech", box, tip, via, base, tail_style), fill=255)
        if tail_style == "fade":
            fades.append((box, tip))
    width = px(float(st["border_mm"] if st["border_mm"] is not None else 0.35), dpi) * scale
    kind = lines[0].balloon or "speech"
    shapes = ImageChops.lighter(mask, bubbles)
    inside = ImageChops.lighter(_erode(mask, width), _erode(bubbles, max(1, width * 2 // 3)))
    band = ImageChops.subtract(shapes, inside)
    if st.get("double"):  # a second line inside the first
        inner = _erode(inside, max(2, width * 2))
        band = ImageChops.lighter(band, ImageChops.subtract(inner, _erode(inner, max(1, width))))
    if fades:  # 消えるしっぽ: its outline and fill thin away toward the tip
        keep = _fade_mask(size, fades)
        band = ImageChops.multiply(band, keep)
        shapes = ImageChops.lighter(ImageChops.multiply(shapes, keep), inside)
    if kind == "whisper":
        band = ImageChops.multiply(band, _dashes(size, [local(b) for b in boxes], scale))
    if kind == "flash":
        band = _flash_lines(size, [local(b) for b in boxes], width)
    if scale > 1:
        full = (rx1 - rx0, ry1 - ry0)
        shapes, band = shapes.resize(full, Image.Resampling.LANCZOS), band.resize(full, Image.Resampling.LANCZOS)
    area = image.crop(region)
    if st["fill"] != "none":
        area.paste(PAPER, (0, 0, area.width, area.height), shapes)
    area.paste(OUTLINE, (0, 0, area.width, area.height), band)
    image.paste(area, (rx0, ry0))


def _dashes(size, boxes, scale: int) -> Image.Image:
    """Angular stripes around each balloon's centre: dashed outlines for whispers."""
    out = Image.new("L", size, 0)
    draw = ImageDraw.Draw(out)
    for x0, y0, x1, y1 in boxes:
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        r = max(x1 - x0, y1 - y0) * 2
        n = max(16, int((x1 - x0 + y1 - y0) / (6 * scale)))
        for i in range(0, n * 2, 2):
            a0, a1 = math.pi * i / n, math.pi * (i + 1) / n
            draw.polygon([(cx, cy), (cx + r * math.cos(a0), cy + r * math.sin(a0)), (cx + r * math.cos(a1), cy + r * math.sin(a1))], fill=255)
    return out


def _flash_lines(size, boxes, width: int) -> Image.Image:
    """Radiating lines around the balloon (a flash): no outline, lines from just inside to outside."""
    out = Image.new("L", size, 0)
    draw = ImageDraw.Draw(out)
    for x0, y0, x1, y1 in boxes:
        cx, cy, rx, ry = (x0 + x1) / 2, (y0 + y1) / 2, (x1 - x0) / 2, (y1 - y0) / 2
        n = max(40, int((rx + ry) / 2))
        for i in range(n):
            t = 2 * math.pi * i / n + (i % 3) * 0.013
            inner = 0.93 + 0.04 * ((i * 7) % 5) / 5
            a = _ellipse_point(cx, cy, rx * inner, ry * inner, t)
            b = _ellipse_point(cx, cy, rx * 1.18, ry * 1.18, t)
            draw.line([a, b], fill=255, width=max(1, width // 2))
    return out


def _paint_text(image: Image.Image, line, dpi: int, show_speaker: bool, font_path: str | None) -> None:
    if style_of(line).get("text_path"):
        path_text(image, line, dpi, font_path)
        return
    text, em = text_image(line, dpi, font_path)
    x, y = px(line.x_mm, dpi), px(line.y_mm, dpi)
    w, h = px(line.w_mm or 40, dpi), px(line.h_mm or 20, dpi)
    cx, cy = x + w / 2, y + h / 2
    if (line.balloon or "speech") == "none":
        image.paste(text, (x, y), text)  # text only: set from the box's corner
    else:
        image.paste(text, (round(cx - text.width / 2), round(cy - text.height / 2)), text)
    if show_speaker and line.speaker and (line.balloon or "speech") != "none":
        font = fonts.face(None).font(max(8, em * 2 // 3))
        ImageDraw.Draw(image).text((x, max(0, y - em)), line.speaker, fill=(90, 90, 90), font=font)


def draw_lines(image: Image.Image, lines: list, dpi: int, font_path: str | None = None, show_speaker: bool = True) -> None:
    """Every placed line of a page, joined balloons drawn together (in reading order of their first line)."""
    groups: dict[str, list] = {}
    order: list[list] = []
    for line in lines:
        key = style_of(line)["group"]
        if key:
            if key not in groups:
                groups[key] = []
                order.append(groups[key])
            groups[key].append(line)
        else:
            order.append([line])
    for group in order:
        draw_group(image, group, dpi, show_speaker, font_path)
