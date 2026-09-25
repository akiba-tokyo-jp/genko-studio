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


ROT = "\x1b"  # a cell that starts with this holds half-width letters laid on their side (縦書きの欧文)
LATIN_LETTERS = frozenset(TCY_HALF - set("!?"))
LATIN_JOINERS = frozenset(" .,'-&/:+")  # may sit inside a run of letters ("Mr. Smith", "E-mail")
LATIN_RUN = 4  # this many letters or more lie on their side; fewer stay upright (2-3 as 縦中横)
MARKS = {"sesame": "sesame", "dot": "dot"}


def _latin_run(text: str, i: int) -> int:
    """The end of a run of half-width letters starting at i (joiners inside, not at the ends)."""
    j = i
    end = i
    while j < len(text) and (text[j] in LATIN_LETTERS or text[j] in LATIN_JOINERS):
        if text[j] in LATIN_LETTERS:
            end = j + 1
        j += 1
    return end


def cells(text: str, tcy: bool = True, latin: bool = False) -> list[str]:
    """The text as vertical cells. With tcy, 2-3 half-width letters or digits ("12", "OK") and
    runs of ！？ ("!?", "!!") sit side by side in one cell (縦中横). With latin, 4 or more half-width
    letters in a row are one cell laid on its side (ROT + the letters). "\n" stays as a cell."""
    out: list[str] = []
    i = 0
    text = text or ""
    while i < len(text):
        char = text[i]
        if latin and char in LATIN_LETTERS:
            j = _latin_run(text, i)
            if sum(1 for c in text[i:j] if c in LATIN_LETTERS) >= LATIN_RUN:
                out.append(ROT + text[i:j])
                i = j
                continue
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


def cell_text(cell: str) -> str:
    return cell[1:] if cell.startswith(ROT) else cell


def columns_of(text: str, per_col: int, tcy: bool = True, latin: bool = False, units=None) -> list[list[str]]:
    """Cells in columns: "\n" starts a column; long runs wrap at per_col rows. Kinsoku: closing marks
    and small kana never start a column (they hang at the end of the one before, ぶら下げ), and opening
    brackets never end one. units(cell) is how many rows a cell takes (a word laid on its side)."""
    per_col = max(1, per_col)
    size = units or (lambda cell: 1)
    cols: list[list[str]] = []
    cur: list[str] = []
    used = 0
    for cell in cells(text, tcy, latin):
        if cell == "\n":
            cols.append(cur)
            cur, used = [], 0
            continue
        need = size(cell)
        if cur and used + need > per_col:
            cols.append(cur)
            cur, used = [], 0
        cur.append(cell)
        used += need
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


def latin_glyph(word: str, font: ImageFont.ImageFont, em: int, fill: tuple[int, int, int]) -> Image.Image:
    """Half-width letters set across, then turned a quarter clockwise to lie along the column."""
    line = Image.new("RGBA", (max(1, int(font.getlength(word)) + em), em), (0, 0, 0, 0))
    try:
        ascent, descent = font.getmetrics()
    except Exception:
        ascent, descent = em, 0
    ImageDraw.Draw(line).text((0, (em - ascent - descent) // 2), word, font=font, fill=fill + (255,))
    box = line.getbbox()
    if box is None:
        return Image.new("RGBA", (em, em), (0, 0, 0, 0))
    line = line.crop((box[0], 0, box[2], em))
    return line.rotate(-90, expand=True)


def _flat_cells(cols: list[list[str]]) -> tuple[str, list[tuple[int, int]]]:
    """The text the cells hold, and for each character its (column, row)."""
    chars: list[str] = []
    where: list[tuple[int, int]] = []
    for c, col in enumerate(cols):
        for r, cell in enumerate(col):
            for char in cell_text(cell):
                chars.append(char)
                where.append((c, r))
    return "".join(chars), where


def _ruby_spans(cols: list[list[str]], ruby_runs: list) -> list[tuple[int, int, int, str]]:
    """(column, first row, last row, ruby) for every ruby run, found in reading order in the text."""
    text, where = _flat_cells(cols)
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
        # a base split over two columns gets its ruby split in proportion
        by_col: dict[int, list[int]] = {}
        for c, r in where[at:at + len(base)]:
            rows = by_col.setdefault(c, [])
            if r not in rows:
                rows.append(r)
        count = sum(len(rows) for rows in by_col.values())
        taken = 0
        for i, (c, rows) in enumerate(sorted(by_col.items())):
            share = len(ruby) - taken if i == len(by_col) - 1 else round(len(ruby) * len(rows) / count)
            spans.append((c, min(rows), max(rows), ruby[taken:taken + share]))
            taken += share
    return spans


def emphasis_cells(cols: list[list[str]], runs: list) -> set[tuple[int, int]]:
    """(column, row) of every cell that carries a 傍点, found in reading order in the text."""
    text, where = _flat_cells(cols)
    out: set[tuple[int, int]] = set()
    pos = 0
    for run in runs or []:
        base = str(run[0] if isinstance(run, (list, tuple)) else run)
        at = text.find(base, pos) if base else -1
        if at < 0:
            continue
        pos = at + len(base)
        for i in range(at, at + len(base)):
            c, r = where[i]
            if not cols[c][r].startswith(ROT) and not text[i].isspace():
                out.add((c, r))
    return out


def draw_mark(image: Image.Image, centre: tuple[float, float], size: float, kind: str, fill, vertical: bool = True) -> None:
    """One 傍点: a sesame (﹅, a filled comma) or a dot (・)."""
    draw = ImageDraw.Draw(image)
    cx, cy = centre
    colour = tuple(fill) + (255,)
    if kind == "dot":
        r = max(1.0, size * 0.2)
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=colour)
        return
    r = max(1.2, size * 0.26)
    # a teardrop: round below, pointed toward the upper right (turned for horizontal text)
    import math

    tip = (cx + r * 1.1, cy - r * 1.6) if vertical else (cx + r * 1.6, cy - r * 1.1)
    points = [tip]
    for k in range(13):
        a = math.radians(-30 + k * 22.5)
        points.append((cx + r * math.cos(a + math.pi / 2), cy + r * math.sin(a + math.pi / 2) * 0.95))
    draw.polygon(points, fill=colour)
    draw.ellipse((cx - r, cy - r * 0.9, cx + r, cy + r), fill=colour)


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
    latin: bool = False,
    emphasis_runs: list | None = None,
    emphasis_mark: str = "sesame",
) -> Image.Image:
    """Vertical text, columns right to left. 傍点 sit right of their characters and ruby right of
    those (ruby moves out when both are there), centred on their base, for every run.

    face: a genko.fonts.Face that picks the font per character (アンチック). tracking / leading:
    extra space between characters / columns, in em. tcy: 縦中横 for short runs of digits and !?.
    latin: 4 or more half-width letters lie on their side. align: top, center or bottom of each
    column in the block."""
    step = max(1, round(em * (1 + tracking)))
    per_col = max(1, (max_height - em) // step + 1) if max_height >= em else 1

    def font_for(char: str):
        return face.font(em, char) if face is not None else font

    turned: dict[str, Image.Image] = {}

    def units(cell: str) -> int:
        if not cell.startswith(ROT):
            return 1
        if cell not in turned:
            image = latin_glyph(cell[1:], font_for("A"), em, fill)
            if image.width > em:
                scale = em / image.width
                image = image.resize((em, max(1, int(image.height * scale))), Image.Resampling.LANCZOS)
            turned[cell] = image
        return max(1, -(-(turned[cell].height - em) // step) + 1)

    cols = columns_of(text, per_col, tcy, latin, units)
    if not cols:
        return Image.new("RGBA", (em, em), (0, 0, 0, 0))
    spans = _ruby_spans(cols, ruby_runs) if ruby_runs else []
    marked = emphasis_cells(cols, emphasis_runs) if emphasis_runs else set()
    ruby_w = max(4, em // 2) if spans else 0
    mark_w = max(3, round(em * 0.36)) if marked else 0
    gap = max(0, round(em * leading))
    pitch = em + mark_w + ruby_w + gap
    width = pitch * len(cols) - gap
    starts = []  # the row each cell starts on, per column
    for col in cols:
        rows, at = [], 0
        for cell in col:
            rows.append(at)
            at += units(cell)
        starts.append(rows + [at])
    height = em + step * (max(rows[-1] for rows in starts) - 1)
    out = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    tops = []
    for index, col in enumerate(cols):
        cx = width - pitch * index - em - mark_w - ruby_w
        col_h = em + step * (starts[index][-1] - 1)
        top = 0 if align == "top" else (height - col_h) // (2 if align == "center" else 1)
        tops.append(top)
        for row, cell in enumerate(col):
            y = top + starts[index][row] * step
            if cell.startswith(ROT):
                image = turned[cell]
                out.alpha_composite(image, (cx + (em - image.width) // 2, y))
                continue
            if len(cell) > 1:
                image = tcy_glyph(cell, font_for("0"), em, fill)
            else:
                image = glyph(cell, font_for(cell), em, fill)
            out.alpha_composite(image, (cx, y))
            if (index, row) in marked:
                draw_mark(out, (cx + em + mark_w / 2, y + em / 2), mark_w, MARKS.get(emphasis_mark, "sesame"), fill)
    for col, first, last, ruby in spans:
        if not ruby:
            continue
        rx = width - pitch * col - ruby_w
        y0, y1 = starts[col][first] * step, starts[col][last] * step
        centre = tops[col] + (y0 + y1 + em) / 2
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
