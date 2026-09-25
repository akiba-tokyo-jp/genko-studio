"""Where a line a person types goes, and how big its balloon is (Qt-free).

A new line goes into the selected panel, vertical, at the panel's top right (manga reads right to
left), to the left of the lines already there. Long text is broken into columns that fit the panel.
"""

from __future__ import annotations

import re as _re

from genko.studio.letter import EM_MM, measure

MARGIN_MM = 3.0

KINDS = [
    ("speech", "普通（楕円）"), ("rounded", "角丸"), ("box", "四角"), ("cloud", "雲（もくもく）"), ("thought", "心の声（泡つき）"),
    ("shout", "叫び（トゲ）"), ("flash", "フラッシュ（放射線）"), ("whisper", "ささやき（点線）"), ("narration", "ナレーション（四角・しっぽなし）"),
    ("sfx", "効果音（描き文字）"), ("none", "文字だけ"),
]
KIND_LABEL = dict(KINDS)


def columns(text: str, per_column: int) -> list[str]:
    """Break text into vertical columns: explicit line breaks first, then by length."""
    out: list[str] = []
    for part in (text or " ").split("\n"):
        part = part or " "
        if len(part) > per_column:  # even columns, as the renderer draws them
            count = -(-len(part) // per_column)
            per_column = -(-len(part) // count)
        while len(part) > per_column:
            out.append(part[:per_column])
            part = part[per_column:]
        out.append(part)
    return out


def box_size(text: str, balloon: str, vertical: bool, max_h_mm: float, max_w_mm: float) -> tuple[float, float]:
    em = EM_MM
    if vertical:
        fits = int((max_h_mm * 0.75) / (em * (1.5 if balloon in ("speech", "thought", "shout", "whisper") else 1.1)))
        per = max(2, min(fits, 4 if balloon == "sfx" else 7))  # a manga column is short: about 7 characters
        w, h = measure(columns(text, per), balloon)
    else:
        per = max(4, int((max_w_mm * 0.8) / em))
        rows = columns(text, per)
        # the same box turned: rows across, characters along
        h, w = measure(rows, balloon)
    return round(min(w, max_w_mm), 2), round(min(h, max_h_mm), 2)


def place_new(episode, page, frame, text: str, balloon: str = "speech", vertical: bool = True) -> dict:
    """x_mm, y_mm, w_mm, h_mm and wrap for a new line in `frame`."""
    r = frame.rect
    w, h = box_size(text, balloon, vertical, r.height - 2 * MARGIN_MM, r.width - 2 * MARGIN_MM)
    right = r.x + r.width - MARGIN_MM
    for line in episode.story_for_page(page.index):
        if line.frame_id == frame.id:
            right = min(right, line.x_mm - 1.5)
    x = max(r.x + MARGIN_MM, right - w)
    return {"x_mm": round(x, 2), "y_mm": round(r.y + MARGIN_MM, 2), "w_mm": w, "h_mm": h,
            "wrap": "vertical" if vertical else "horizontal"}


def refit(line, frame, text: str, balloon: str, vertical: bool) -> dict:
    """New size after the text or kind changed, keeping the balloon's top-right corner."""
    max_h = (frame.rect.height - 2 * MARGIN_MM) if frame else 120.0
    max_w = (frame.rect.width - 2 * MARGIN_MM) if frame else 120.0
    w, h = box_size(text, balloon, vertical, max_h, max_w)
    right = line.x_mm + line.w_mm
    return {"x_mm": round(right - w, 2), "y_mm": line.y_mm, "w_mm": w, "h_mm": h}


# --- ruby in the text box: ｜約束《やくそく》 (the notation Japanese novel sites use) -------------------


_KANJI = r"[㐀-鿿豈-﫿々〆ヶ]"
_RUBY = _re.compile(r"[｜|]([^｜|《》\n]+)《([^《》\n]+)》|(" + _KANJI + r"+)《([^《》\n]+)》")


def parse_ruby(text: str) -> tuple[str, list[list[str]]]:
    """'｜約束《やくそく》の日' → ('約束の日', [['約束', 'やくそく']]). Kanji right before 《》 need no ｜."""
    runs: list[list[str]] = []

    def take(match) -> str:
        base = match.group(1) or match.group(3)
        runs.append([base, match.group(2) or match.group(4)])
        return base

    return _RUBY.sub(take, text or ""), runs


_EMPHASIS = _re.compile(r"《《([^《》\n]+)》》")


_STYLED = _re.compile(r"[{｛]([^{}｛｝|｜\n]+)[|｜]([^{}｛｝\n]+)[}｝]")


def parse_styles(typed: str) -> tuple[str, list]:
    """Part of a line styled as typed: '{大|なんだと}' → ('なんだと', [['なんだと', {'scale': 1.4}]]).
    Marks: 大・特大・小・太・赤・青・白, several with 、 or , ('{大、太|…}'). Unknown marks stay as typed."""
    from genko.tategaki import STYLE_TAGS

    styles: list = []

    def take(match) -> str:
        tags = [t.strip() for t in _re.split(r"[、,，・ ]+", match.group(1)) if t.strip()]
        if not tags or any(t not in STYLE_TAGS for t in tags):
            return match.group(0)
        style: dict = {}
        for tag in tags:
            style.update(STYLE_TAGS[tag])
        styles.append([match.group(2), style])
        return match.group(2)

    return _STYLED.sub(take, typed or ""), styles


def parse_marks(typed: str) -> tuple[str, list[list[str]], list[str], list]:
    """Ruby, 傍点 and styled parts as typed: '《《絶対》》に｜約束《やくそく》{大|する}' →
    ('絶対に約束する', [['約束', 'やくそく']], ['絶対'], [['する', {'scale': 1.4}]])
    (《《…》》 for dots, as on Japanese novel sites; {大|…} for a part larger, smaller, bolder or coloured)."""
    emphasis: list[str] = []

    def take(match) -> str:
        emphasis.append(match.group(1))
        return match.group(1)

    styled, styles = parse_styles(typed)
    text, runs = parse_ruby(_EMPHASIS.sub(take, styled))
    # (the styled words, found again in the text without its marks)
    styles = [[parse_ruby(_EMPHASIS.sub(lambda m: m.group(1), words))[0], style] for words, style in styles]
    return text, runs, emphasis, styles


def with_marks(line) -> str:
    """A line as typed back: its ruby, 傍点 and styled parts in the notation."""
    from genko.tategaki import STYLE_TAGS

    text = with_ruby(line.text, line.ruby_runs)
    pos = 0
    for base in getattr(line, "emphasis_runs", None) or []:
        at = text.find(base, pos)
        if at < 0:
            continue
        text = text[:at] + f"《《{base}》》" + text[at + len(base):]
        pos = at + len(base) + 4
    pos = 0
    for words, style in getattr(line, "style_runs", None) or []:
        tags = []
        for tag, value in STYLE_TAGS.items():
            if all(style.get(k) == v for k, v in value.items()):
                tags.append(tag)
        at = text.find(words, pos)
        if at < 0 or not tags:
            continue
        mark = "、".join(tags)
        text = text[:at] + f"{{{mark}|{words}}}" + text[at + len(words):]
        pos = at + len(words) + len(mark) + 3
    return text


def with_ruby(text: str, runs) -> str:
    """The text as typed back, with its ruby in the notation (each run once, in order)."""
    out, pos = [], 0
    for base, ruby in runs or []:
        at = (text or "").find(base, pos)
        if at < 0:
            continue
        out.append(text[pos:at])
        out.append(f"｜{base}《{ruby}》")
        pos = at + len(base)
    out.append((text or "")[pos:])
    return "".join(out)


def place_at(x_mm: float, y_mm: float, text: str, balloon: str = "speech", vertical: bool = True,
             frame=None) -> dict:
    """A balloon centred where the person clicked, sized to its text and kept inside the panel."""
    max_h = (frame.rect.height - 2 * MARGIN_MM) if frame else 120.0
    max_w = (frame.rect.width - 2 * MARGIN_MM) if frame else 160.0
    w, h = box_size(text, balloon, vertical, max_h, max_w)
    x, y = x_mm - w / 2, y_mm - h / 2
    if frame is not None:
        r = frame.rect
        x = max(r.x + 1, min(r.x + r.width - w - 1, x))
        y = max(r.y + 1, min(r.y + r.height - h - 1, y))
    return {"x_mm": round(x, 2), "y_mm": round(y, 2), "w_mm": w, "h_mm": h, "wrap": "vertical" if vertical else "horizontal"}
