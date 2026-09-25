"""Where a line a person types goes, and how big its balloon is (Qt-free).

A new line goes into the selected panel, vertical, at the panel's top right (manga reads right to
left), to the left of the lines already there. Long text is broken into columns that fit the panel.
"""

from __future__ import annotations

from genko.studio.letter import EM_MM, measure

MARGIN_MM = 3.0

KINDS = [
    ("speech", "普通のフキダシ"), ("shout", "叫び（トゲ）"), ("thought", "心の声（もくもく）"), ("whisper", "ささやき（点線）"),
    ("narration", "ナレーション（四角）"), ("sfx", "効果音（描き文字）"), ("none", "フキダシなし（文字だけ）"),
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
