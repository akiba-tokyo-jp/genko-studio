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


def _columns(text: str, per_col: int) -> list[list[str]]:
    if per_col < 1:
        per_col = 1
    cols: list[list[str]] = []
    # An explicit "\n" always starts a new column; long segments still wrap at per_col.
    for segment in (text or "").split("\n"):
        cur: list[str] = []
        for char in segment:
            if len(cur) >= per_col:
                cols.append(cur)
                cur = []
            cur.append(char)
        if cur:
            cols.append(cur)
    for i in range(1, len(cols)):
        while cols[i] and cols[i][0] in LINE_START_KINSOKU and cols[i - 1]:
            cols[i - 1].append(cols[i].pop(0))
    return [col for col in cols if col]


def compose(
    text: str,
    font: ImageFont.ImageFont,
    em: int,
    max_height: int,
    fill: tuple[int, int, int] = (10, 10, 10),
    ruby_runs: list | None = None,
) -> Image.Image:
    per_col = max(1, max_height // em)
    cols = _columns(text, per_col)
    if not cols:
        return Image.new("RGBA", (em, em), (0, 0, 0, 0))
    ruby_w = em // 2 if ruby_runs else 0
    width = em * len(cols) + ruby_w
    height = em * max(len(col) for col in cols)
    out = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    for index, col in enumerate(cols):
        cx = width - ruby_w - em * (index + 1)
        for row, char in enumerate(col):
            out.alpha_composite(glyph(char, font, em, fill), (cx, row * em))
    if ruby_runs:
        ruby_font_size = max(8, em // 2)
        try:
            ruby_font = font.font_variant(size=ruby_font_size)  # type: ignore[attr-defined]
        except Exception:
            ruby_font = font
        first = ruby_runs[0]
        ruby = first[1] if len(first) > 1 else ""
        rx = width - ruby_w
        for i, char in enumerate(ruby):
            out.alpha_composite(glyph(char, ruby_font, ruby_w, fill), (rx, i * ruby_w))
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
