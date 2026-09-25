from __future__ import annotations

from PIL import Image, ImageDraw, ImageFont

SMALL_KANA = frozenset("ぁぃぅぇぉっゃゅょゎゕゖァィゥェォッャュョヮヵヶ")
PUNCT_TR = frozenset("、。，．")
ROTATE_CW = frozenset("ー―─━−‐–—〜～～＝=…‥:：;；〰︱")
VERTICAL_FORMS = {
    "「": "﹁",
    "」": "﹂",
    "『": "﹃",
    "』": "﹄",
    "（": "︵",
    "）": "︶",
    "(": "︵",
    ")": "︶",
    "【": "︻",
    "】": "︼",
    "［": "﹇",
    "］": "﹈",
    "〈": "︿",
    "〉": "﹀",
    "《": "︽",
    "》": "︾",
}
LINE_START_KINSOKU = frozenset("、。，．）」』)】］〉》ーぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮ")


def _textbbox(font: ImageFont.ImageFont, char: str) -> tuple[int, int, int, int]:
    probe = ImageDraw.Draw(Image.new("L", (8, 8)))
    try:
        return probe.textbbox((0, 0), char, font=font)
    except Exception:
        size = int(getattr(font, "size", 14) or 14)
        return (0, 0, size, size)


def _has_glyph(font: ImageFont.ImageFont, char: str) -> bool:
    try:
        box = font.getmask(char).getbbox()
    except Exception:
        return False
    return box is not None and (box[2] - box[0]) > 1 and (box[3] - box[1]) > 1


def _ink_bbox(font: ImageFont.ImageFont, char: str) -> tuple[int, int, int, int]:
    try:
        box = font.getmask(char).getbbox()
    except Exception:
        box = None
    if box:
        return box
    return _textbbox(font, char)


def _raw_glyph(char: str, font: ImageFont.ImageFont, fill: tuple[int, int, int]) -> Image.Image:
    size = max(16, int(getattr(font, "size", 14) or 14))
    canvas = Image.new("RGBA", (size * 4, size * 4), (0, 0, 0, 0))
    ImageDraw.Draw(canvas).text((size, size), char, font=font, fill=fill + (255,))
    bbox = canvas.getbbox()
    if bbox is None:
        return Image.new("RGBA", (size // 2, size // 2), (0, 0, 0, 0))
    return canvas.crop(bbox)


def glyph(char: str, font: ImageFont.ImageFont, em: int, fill: tuple[int, int, int]) -> Image.Image:
    img = Image.new("RGBA", (em, em), (0, 0, 0, 0))
    rotate = char in ROTATE_CW
    drawn = char
    form = VERTICAL_FORMS.get(char)
    if form and _has_glyph(font, form):
        drawn = form
        rotate = False
    elif char in VERTICAL_FORMS:
        rotate = True
    raw = _raw_glyph(drawn, font, fill)
    gw, gh = raw.size
    margin = max(1, em // 12)
    if drawn in PUNCT_TR:
        ox = max(0, em - margin - gw)
        oy = margin
        img.paste(raw, (ox, oy), raw)
        return img
    if drawn in SMALL_KANA:
        # Some fonts (e.g. the bundled Dela Gothic) draw small kana nearly full size;
        # shrink them so they read as small and sit in the upper right of the em box.
        limit = int(em * 0.62)
        if max(gw, gh) > limit:
            scale = limit / max(gw, gh)
            raw = raw.resize((max(1, int(gw * scale)), max(1, int(gh * scale))), Image.Resampling.LANCZOS)
            gw, gh = raw.size
        ox = max(0, em - margin - gw)
        oy = max(0, (em - gh) // 2 - em // 10)
        img.paste(raw, (ox, oy), raw)
        return img
    ox = max(0, (em - gw) // 2)
    oy = max(0, (em - gh) // 2)
    img.paste(raw, (ox, oy), raw)
    if rotate:
        img = img.rotate(-90, resample=Image.Resampling.BICUBIC, expand=False)
    return img


LINE_END_KINSOKU = frozenset("「『（(【［〈《〔｛")
TCY_HALF = frozenset("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!?")
FULL_DIGITS = {chr(0xFF10 + i): str(i) for i in range(10)}
BANGS = {"！": "!", "？": "?", "!": "!", "?": "?"}


def cells(text: str, tcy: bool = True) -> list[str]:
    """The text as vertical cells. With tcy, 2-3 half-width letters or digits ("12", "OK") and
    runs of ！？ ("!?", "!!") sit side by side in one cell (縦中横). "\n" stays as a cell."""
    out: list[str] = []
    i = 0
    text = text or ""
    while i < len(text):
        char = text[i]
        if tcy and char in BANGS:
            j = i
            while j < len(text) and text[j] in BANGS and j - i < 3:
                j += 1
            if j - i >= 2:
                out.append("".join(BANGS[c] for c in text[i:j]))
                i = j
                continue
        if tcy and char in FULL_DIGITS:
            j = i
            while j < len(text) and text[j] in FULL_DIGITS:
                j += 1
            if 2 <= j - i <= 3:  # "１２" reads as one number: side by side, like "12"
                out.append("".join(FULL_DIGITS[c] for c in text[i:j]))
                i = j
                continue
            out.extend(text[i:j])
            i = j
            continue
        if tcy and char in TCY_HALF and char not in "!?":
            j = i
            while j < len(text) and text[j] in TCY_HALF and text[j] not in "!?":
                j += 1
            if 2 <= j - i <= 3:
                out.append(text[i:j])
                i = j
                continue
            out.extend(text[i:j])
            i = j
            continue
        out.append(char)
        i += 1
    return out


def columns_of(text: str, per_col: int, tcy: bool = True) -> list[list[str]]:
    """Cells in columns: "\n" starts a column; long runs wrap at per_col. Kinsoku: closing marks and
    small kana never start a column (they hang at the end of the one before, ぶら下げ), and opening
    brackets never end one."""
    per_col = max(1, per_col)
    cols: list[list[str]] = []
    cur: list[str] = []
    for cell in cells(text, tcy):
        if cell == "\n":
            cols.append(cur)
            cur = []
            continue
        if len(cur) >= per_col:
            cols.append(cur)
            cur = []
        cur.append(cell)
    cols.append(cur)
    for i in range(1, len(cols)):
        while cols[i] and cols[i][0] in LINE_START_KINSOKU and cols[i - 1]:
            cols[i - 1].append(cols[i].pop(0))
    for i in range(len(cols) - 1):
        while cols[i] and cols[i][-1] in LINE_END_KINSOKU and len(cols[i]) > 1:
            cols[i + 1].insert(0, cols[i].pop())
    return [col for col in cols if col]


def _columns(text: str, per_col: int) -> list[list[str]]:
    return columns_of(text, per_col, tcy=False)


def tcy_glyph(cell: str, font: ImageFont.ImageFont, em: int, fill: tuple[int, int, int]) -> Image.Image:
    """Two or three characters side by side in one em (condensed to fit)."""
    raw = _raw_glyph(cell, font, fill)
    gw, gh = raw.size
    limit = int(em * 0.92)
    scale = min(1.0, limit / max(1, gw), limit / max(1, gh))
    if scale < 1.0:
        raw = raw.resize((max(1, int(gw * scale)), max(1, int(gh * scale))), Image.Resampling.LANCZOS)
    img = Image.new("RGBA", (em, em), (0, 0, 0, 0))
    img.paste(raw, ((em - raw.width) // 2, (em - raw.height) // 2), raw)
    return img


def _ruby_spans(cols: list[list[str]], ruby_runs: list) -> list[tuple[int, int, int, str]]:
    """(column, first row, last row, ruby) for every ruby run, found in reading order in the text."""
    flat: list[tuple[int, int, str]] = [(c, r, ch) for c, col in enumerate(cols) for r, ch in enumerate(col)]
    text = "".join(ch for _, _, ch in flat)
    spans: list[tuple[int, int, int, str]] = []
    pos = 0
    for run in ruby_runs or []:
        if not run or len(run) < 2 or not run[0] or not run[1]:
            continue
        base, ruby = str(run[0]), str(run[1])
        at = text.find(base, pos)
        if at < 0:
            continue
        pos = at + len(base)
        cells = flat[at:at + len(base)]
        # a base split over two columns gets its ruby split in proportion
        by_col: dict[int, list[int]] = {}
        for c, r, _ in cells:
            by_col.setdefault(c, []).append(r)
        taken = 0
        for i, (c, rows) in enumerate(sorted(by_col.items())):
            share = len(ruby) - taken if i == len(by_col) - 1 else round(len(ruby) * len(rows) / len(cells))
            spans.append((c, min(rows), max(rows), ruby[taken:taken + share]))
            taken += share
    return spans


def compose(
    text: str,
    font: ImageFont.ImageFont,
    em: int,
    max_height: int,
    fill: tuple[int, int, int] = (10, 10, 10),
    ruby_runs: list | None = None,
    *,
    face=None,
    tracking: float = 0.0,
    leading: float = 0.0,
    tcy: bool = False,
    align: str = "top",
) -> Image.Image:
    """Vertical text, columns right to left. Ruby sits to the right of its base characters,
    centred on them, for every run.

    face: a genko.fonts.Face that picks the font per character (アンチック). tracking / leading:
    extra space between characters / columns, in em. tcy: 縦中横 for short runs of digits and !?.
    align: top, center or bottom of each column in the block."""
    step = max(1, round(em * (1 + tracking)))
    per_col = max(1, (max_height - em) // step + 1) if max_height >= em else 1
    cols = columns_of(text, per_col, tcy)
    if not cols:
        return Image.new("RGBA", (em, em), (0, 0, 0, 0))
    spans = _ruby_spans(cols, ruby_runs) if ruby_runs else []
    ruby_w = max(4, em // 2) if spans else 0
    gap = max(0, round(em * leading))
    pitch = em + ruby_w + gap
    width = pitch * len(cols) - gap
    height = em + step * (max(len(col) for col in cols) - 1)
    out = Image.new("RGBA", (width, height), (0, 0, 0, 0))

    def font_for(char: str):
        return face.font(em, char) if face is not None else font

    for index, col in enumerate(cols):
        cx = width - pitch * index - em - ruby_w
        col_h = em + step * (len(col) - 1)
        top = 0 if align == "top" else (height - col_h) // (2 if align == "center" else 1)
        for row, cell in enumerate(col):
            if len(cell) > 1:
                image = tcy_glyph(cell, font_for("0"), em, fill)
            else:
                image = glyph(cell, font_for(cell), em, fill)
            out.alpha_composite(image, (cx, top + row * step))
    if spans:
        for col, first, last, ruby in spans:
            if not ruby:
                continue
            rx = width - pitch * col - ruby_w
            centre = (first * step + last * step + em) / 2
            top = max(0, min(height - ruby_w * len(ruby), round(centre - ruby_w * len(ruby) / 2)))
            for i, char in enumerate(ruby):
                base = font_for(char)
                try:
                    ruby_font = base.font_variant(size=max(8, ruby_w))  # type: ignore[attr-defined]
                except Exception:
                    ruby_font = base
                out.alpha_composite(glyph(char, ruby_font, ruby_w, fill), (rx, top + i * ruby_w))
    return out


def paste_vertical(
    page: Image.Image,
    text: str,
    box: tuple[int, int, int, int],
    font: ImageFont.ImageFont,
    fill: tuple[int, int, int] = (10, 10, 10),
    ruby_runs: list | None = None,
) -> tuple[int, int, int, int]:
    x, y, x2, y2 = box
    em = max(8, int(getattr(font, "size", 14) or 14))
    max_h = max(em, y2 - y)
    composed = compose(text, font, em, max_h, fill=fill, ruby_runs=ruby_runs)
    page.paste(composed, (x, y), composed)
    return (x, y, x + composed.width, y + composed.height)
