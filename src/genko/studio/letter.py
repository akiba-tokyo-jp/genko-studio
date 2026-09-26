"""Lettering v1: size vertical balloons and place them inside their panels.

Sizes follow the renderer (render._draw_balloon): the em is capped at 5 mm,
text columns run right to left and the balloon adds a quarter-em pad. The
result becomes add_line ops with explicit coordinates, so a replay never
depends on fonts.
"""

from __future__ import annotations

from dataclasses import dataclass

from genko.studio.blocking import Box, Figure, figures
from genko.studio.issues import Issue, error, warning
from genko.studio.layout import CompiledLayout

EM_MM = 5.0  # render caps the vertical balloon font at 5 mm
PAD_MM = EM_MM / 4
MARGIN_MM = 2.0
GAP_MM = 1.0
STEP_MM = 1.5
TAIL_MM = 7.0


@dataclass(frozen=True)
class BalloonPlacement:
    slot: str
    line_index: int
    beat_id: str
    text: str
    balloon: str
    speaker: str
    x_mm: float
    y_mm: float
    w_mm: float
    h_mm: float
    tail: tuple[float, float] | None


ELLIPSE_KINDS = ("speech", "thought", "shout", "whisper")
LEADING = 0.15
SFX_EM_MM = 12.0  # the renderer's largest SFX glyph
SPIKED = {"shout": 0.26}  # (a shout's default spike depth, and a little more for its valleys' chords)


def measure(breaks: list[str], balloon: str) -> tuple[float, float]:
    """The balloon's box as the renderer draws it: text block + pad, and for ellipses the ellipse
    that passes around the text block's corners (half-size × √2)."""
    cols = [b for b in breaks if b] or [" "]
    longest = max(len(b) for b in cols)
    if balloon == "sfx":
        return (SFX_EM_MM * len(cols), SFX_EM_MM * longest)
    # columns are LEADING em apart (the renderer's default), characters sit edge to edge
    text_w, text_h = EM_MM * len(cols) + EM_MM * LEADING * (len(cols) - 1), EM_MM * longest
    if balloon in ELLIPSE_KINDS or balloon in SPIKED:
        # the text's corners on an ellipse (half-size × √2); a spiked edge's valleys cut in by its depth, so the
        # ellipse the text needs is the valleys', and the outline is that much bigger
        grow = 2 ** 0.5 / (1 - SPIKED.get(balloon, 0.0))
        return (text_w * grow + 2 * PAD_MM, text_h * grow + 2 * PAD_MM)
    pad = 0.0 if balloon == "none" else PAD_MM
    return (text_w + 2 * pad, text_h + 2 * pad)


def fits(rect: Box, balloon: str, others: int = 0) -> str:
    """How much text this panel takes in one balloon of this kind, in words for the agent."""
    _x, _y, w, h = rect
    room_w, room_h = w - 2 * MARGIN_MM, h - 2 * MARGIN_MM
    if balloon == "sfx":
        chars, cols = int(room_h // SFX_EM_MM), int(room_w // SFX_EM_MM)
    else:
        grow = 2 ** 0.5 / (1 - SPIKED.get(balloon, 0.0)) if (balloon in ELLIPSE_KINDS or balloon in SPIKED) else 1.0
        pad = 0.0 if balloon == "none" else PAD_MM
        chars = int(((room_h - 2 * pad) / grow) // EM_MM)
        cols = int((((room_w - 2 * pad) / grow) + EM_MM * LEADING) // (EM_MM * (1 + LEADING)))
    where = f"このコマ（{w:.0f}×{h:.0f} mm）のこの形のフキダシには、1 列 {max(0, chars)} 字・{max(0, cols)} 列まで"
    return where + ("（ほかの台詞と場所を分け合うので、実際はもっと少ない）" if others else "")


def place_page(
    plan: dict, layout: CompiledLayout, bible: dict, speakers: dict[str, str | None]
) -> tuple[list[BalloonPlacement], list[Issue]]:
    """`speakers` maps beat id to the speaking character id (from the script)."""
    names = {c.get("id"): c.get("name", "") for c in bible.get("characters", [])}
    placements: list[BalloonPlacement] = []
    issues: list[Issue] = []
    for pi, panel in enumerate(plan.get("panels", [])):
        rect = layout.leaf_rects_mm.get(panel.get("slot"))
        if rect is None:
            continue
        figs = figures(panel, rect)
        placed: list[Box] = []
        for li, line in enumerate(panel.get("lines", [])):
            path = f"/panels/{pi}/lines/{li}"
            size = measure(line.get("breaks", []), line.get("balloon", "speech"))
            spot = _find_spot(rect, size, placed, figs)
            if spot is None:
                issues.append(
                    error(
                        "balloon_overflow",
                        path,
                        f"コマ {panel.get('slot')} に台詞が入らない（{size[0]:.0f}×{size[1]:.0f} mm）",
                        f"{fits(rect, line.get('balloon', 'speech'), len(placed))}。台詞を短くする、breaks で列を分ける、"
                        "台詞を別のコマに移す、コマを大きくする",
                    )
                )
                continue
            box, covers_face = spot
            if covers_face:
                issues.append(warning("balloon_covers_face", path, f"コマ {panel.get('slot')} で台詞が顔にかかる", "台詞を減らすか、人物の pos を変える"))
            placed.append(box)
            speaker_id = speakers.get(line.get("beat_id"))
            placements.append(
                BalloonPlacement(
                    slot=panel["slot"],
                    line_index=li,
                    beat_id=line.get("beat_id", ""),
                    text="\n".join(line.get("breaks", [])),
                    balloon=line.get("balloon", "speech"),
                    speaker=names.get(speaker_id, "") if speaker_id else "",
                    x_mm=round(box[0], 2),
                    y_mm=round(box[1], 2),
                    w_mm=round(box[2], 2),
                    h_mm=round(box[3], 2),
                    tail=_tail(box, line.get("balloon", "speech"), speaker_id, figs),
                )
            )
    return placements, issues


def placements_to_ops(page_index: int, placements: list[BalloonPlacement], layout: CompiledLayout) -> list[dict]:
    ops = []
    for p in placements:
        op = {
            "op": "add_line",
            "page": page_index,
            "text": p.text,
            # The renderer prints the speaker name above the balloon (also in print), so leave it empty.
            "speaker": "",
            "frame_id": layout.slot_to_frame[p.slot],
            "balloon": p.balloon,
            "wrap": "vertical",
            "x_mm": p.x_mm,
            "y_mm": p.y_mm,
            "w_mm": p.w_mm,
            "h_mm": p.h_mm,
        }
        if p.tail:
            op["tail"] = [round(p.tail[0], 2), round(p.tail[1], 2)]
        ops.append(op)
    return ops


def _find_spot(rect: Box, size: tuple[float, float], placed: list[Box], figs: list[Figure]) -> tuple[Box, bool] | None:
    x0, y0, w, h = rect
    bw, bh = size
    left, top = x0 + MARGIN_MM, y0 + MARGIN_MM
    right, bottom = x0 + w - MARGIN_MM, y0 + h - MARGIN_MM
    if bw > right - left or bh > bottom - top:
        return None
    prev = placed[-1] if placed else None
    best: tuple[float, Box, bool] | None = None
    y = top
    while y + bh <= bottom + 1e-6:
        x = right - bw
        while x >= left - 1e-6:
            box = (x, y, bw, bh)
            if not any(_overlap(_grow(box, GAP_MM), other) > 0 for other in placed) and _after(box, prev):
                face = sum(_overlap(box, f.head) for f in figs)
                body = sum(_overlap(box, f.body) for f in figs)
                cost = 20000.0 * face / (bw * bh) + 3.0 * body / (bw * bh) + (right - (x + bw)) + 1.2 * (y - top)
                if best is None or cost < best[0]:
                    best = (cost, box, face > 0)
            x -= STEP_MM
        y += STEP_MM
    if best is None:
        return None
    return best[1], best[2]


def _after(box: Box, prev: Box | None) -> bool:
    """Japanese reading order: the next balloon sits to the left of, or below, the previous one."""
    if prev is None:
        return True
    x, y, w, h = box
    px, py, pw, ph = prev
    return x + w <= px + pw * 0.5 or y >= py + ph * 0.5


def _tail(box: Box, balloon: str, speaker_id: str | None, figs: list[Figure]) -> tuple[float, float] | None:
    if balloon in ("narration", "thought", "sfx") or not speaker_id:
        return None
    fig = next((f for f in figs if f.char_id == speaker_id), None)
    if fig is None:
        return None
    x, y, w, h = box
    sx, sy = x + w / 2, y + h
    hx, hy, hw, hh = fig.head
    tx, ty = hx + hw / 2, hy + hh / 2
    dx, dy = tx - sx, ty - sy
    dist = (dx * dx + dy * dy) ** 0.5
    if dist < 1e-6:
        return None
    step = min(TAIL_MM, dist * 0.6)
    return (sx + dx / dist * step, sy + dy / dist * step)


def _grow(box: Box, by: float) -> Box:
    x, y, w, h = box
    return (x - by, y - by, w + 2 * by, h + 2 * by)


def _overlap(a: Box, b: Box) -> float:
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    dx = min(ax + aw, bx + bw) - max(ax, bx)
    dy = min(ay + ah, by + bh) - max(ay, by)
    return dx * dy if dx > 0 and dy > 0 else 0.0
