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
TAIL_ROOM_MM = 3.0  # (a balloon keeps this far from its speaker's face: the tail's point goes between)
TAIL_MAX_MM = 30.0  # (a tail longer than this past the balloon's edge reads as a line of its own)


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
    speaker_id: str = ""
    style: dict | None = None


ELLIPSE_KINDS = ("speech", "thought", "shout", "whisper")
LEADING = 0.15
SFX_EM_MM = 12.0  # the renderer's largest SFX glyph
SPIKED = {"shout": 0.26}
# (when the bible says nothing: a shout is bigger and bold, a whisper smaller; fonts stay the book's own)
DEFAULT_LOOKS: dict[str, dict] = {"shout": {"scale": 1.3, "weight": "bold"}, "whisper": {"scale": 0.8}}
TITLE_MAX_MM = 18.0  # (the title's letters at most; the author's name is a third of that, 5 mm at least)


def looks(bible: dict | None) -> dict[str, dict]:
    """Each kind of text's font, size (times the usual) and weight: the defaults, then the bible's lettering."""
    out = {kind: dict(look) for kind, look in DEFAULT_LOOKS.items()}
    for kind, look in ((bible or {}).get("lettering") or {}).items():
        if isinstance(look, dict):
            out[kind] = {**out.get(kind, {}), **{k: v for k, v in look.items() if v is not None}}
    return out


def look_style(look: dict, balloon: str) -> dict:
    """The line style that carries a look (size_mm only when the size changes)."""
    style: dict = {}
    if look.get("font"):
        style["font"] = str(look["font"])
    if look.get("weight") and look["weight"] != "normal":
        style["weight"] = str(look["weight"])
    scale = float(look.get("scale") or 1.0)
    if balloon != "sfx" and abs(scale - 1.0) > 1e-3:
        style["size_mm"] = round(EM_MM * scale, 2)
    return style  # (a shout's default spike depth, and a little more for its valleys' chords)


def measure(breaks: list[str], balloon: str, em: float = EM_MM) -> tuple[float, float]:
    """The balloon's box as the renderer draws it: text block + pad, and for ellipses the ellipse
    that passes around the text block's corners (half-size × √2)."""
    cols = [b for b in breaks if b] or [" "]
    longest = max(len(b) for b in cols)
    if balloon == "sfx":
        return (SFX_EM_MM * len(cols), SFX_EM_MM * longest)
    # columns are LEADING em apart (the renderer's default), characters sit edge to edge
    text_w, text_h = em * len(cols) + em * LEADING * (len(cols) - 1), em * longest
    if balloon in ELLIPSE_KINDS or balloon in SPIKED:
        # the text's corners on an ellipse (half-size × √2); a spiked edge's valleys cut in by its depth, so the
        # ellipse the text needs is the valleys', and the outline is that much bigger
        grow = 2 ** 0.5 / (1 - SPIKED.get(balloon, 0.0))
        return (text_w * grow + 2 * em / 4, text_h * grow + 2 * em / 4)
    pad = 0.0 if balloon == "none" else em / 4
    return (text_w + 2 * pad, text_h + 2 * pad)


def fits(rect: Box, balloon: str, others: int = 0, em: float = EM_MM) -> str:
    """How much text this panel takes in one balloon of this kind, in words for the agent."""
    _x, _y, w, h = rect
    room_w, room_h = w - 2 * MARGIN_MM, h - 2 * MARGIN_MM
    if balloon == "sfx":
        chars, cols = int(room_h // SFX_EM_MM), int(room_w // SFX_EM_MM)
    else:
        grow = 2 ** 0.5 / (1 - SPIKED.get(balloon, 0.0)) if (balloon in ELLIPSE_KINDS or balloon in SPIKED) else 1.0
        pad = 0.0 if balloon == "none" else em / 4
        chars = int(((room_h - 2 * pad) / grow) // em)
        cols = int((((room_w - 2 * pad) / grow) + em * LEADING) // (em * (1 + LEADING)))
    where = f"このコマ（{w:.0f}×{h:.0f} mm）のこの形のフキダシには、1 列 {max(0, chars)} 字・{max(0, cols)} 列まで"
    return where + ("（ほかの台詞と場所を分け合うので、実際はもっと少ない）" if others else "")


def place_page(
    plan: dict, layout: CompiledLayout, bible: dict, speakers: dict[str, str | None]
) -> tuple[list[BalloonPlacement], list[Issue]]:
    """`speakers` maps beat id to the speaking character id (from the script)."""
    names = {c.get("id"): c.get("name", "") for c in bible.get("characters", [])}
    kinds = looks(bible)
    title_slot = None
    if plan.get("title"):  # (扉: the title and the author's name, set before the lines in their panel)
        shape = {} if plan.get("tiers") else _template_shape(plan.get("template"))
        title_slot = shape.get("title") or (layout.reading_order[0] if layout.reading_order else None)
    placements: list[BalloonPlacement] = []
    issues: list[Issue] = []
    for pi, panel in enumerate(plan.get("panels", [])):
        rect = layout.leaf_rects_mm.get(panel.get("slot"))
        if rect is None:
            continue
        figs = figures(panel, rect)
        placed: list[Box] = []
        if panel.get("slot") == title_slot:
            for item in _title_placements(panel["slot"], rect, bible, kinds.get("title", {})):
                placements.append(item)
                placed.append((item.x_mm, item.y_mm, item.w_mm, item.h_mm))
        for li, line in enumerate(panel.get("lines", [])):
            path = f"/panels/{pi}/lines/{li}"
            balloon = line.get("balloon", "speech")
            style = look_style(kinds.get(balloon, {}), balloon)
            em = float(style.get("size_mm") or EM_MM)
            size = measure(line.get("breaks", []), balloon, em)
            speaker_id = speakers.get(line.get("beat_id"))
            near = None
            where = panel.get("sfx_at")
            if balloon == "sfx" and isinstance(where, (list, tuple)) and len(where) == 2:  # (the sound's source, 0..1)
                near = (rect[0] + rect[2] * min(1.0, max(0.0, float(where[0]))), rect[1] + rect[3] * min(1.0, max(0.0, float(where[1]))))
            spot = _find_spot(rect, size, placed, figs, speaker_id, near)
            if spot is None:
                issues.append(
                    error(
                        "balloon_overflow",
                        path,
                        f"コマ {panel.get('slot')} に台詞が入らない（{size[0]:.0f}×{size[1]:.0f} mm）",
                        f"{fits(rect, balloon, len(placed), em)}。台詞を短くする、breaks で列を分ける、"
                        "台詞を別のコマに移す、コマを大きくする",
                    )
                )
                continue
            box, covers_face = spot
            if covers_face:
                issues.append(warning("balloon_covers_face", path, f"コマ {panel.get('slot')} で台詞が顔にかかる", "台詞を減らすか、人物の pos を変える"))
            placed.append(box)
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
                    speaker_id=speaker_id or "",
                    style=style or None,
                )
            )
    return placements, issues


def _template_shape(name: str | None) -> dict:
    from genko.studio.layout import templates

    return templates().get(name or "", {})


def _title_placements(slot: str, rect: Box, bible: dict, look: dict) -> list[BalloonPlacement]:
    """The title down the panel's right edge, as large as it fits (TITLE_MAX_MM at most), and the author's name in
    a narrow column beside it, at its foot."""
    title = str(bible.get("title") or "").strip()
    if not title:
        return []
    x, y, w, h = rect
    room = h - 2 * MARGIN_MM
    em = max(6.0, min(TITLE_MAX_MM, room * 0.85 / max(1, len(title)), w * 0.35))
    style = {k: v for k, v in look_style({**look, "scale": None}, "none").items()}
    title_box = (x + w - MARGIN_MM - em * 1.1, y + MARGIN_MM, em * 1.1, min(room, em * len(title) * 1.02))
    out = [BalloonPlacement(slot=slot, line_index=-1, beat_id="", text=title, balloon="none", speaker="", x_mm=round(title_box[0], 2),
                            y_mm=round(title_box[1], 2), w_mm=round(title_box[2], 2), h_mm=round(title_box[3], 2), tail=None,
                            style={**style, "size_mm": round(em, 2)})]
    author = str(bible.get("author") or "").strip()
    if author:
        small = max(5.0, round(em / 3, 2))
        height = min(room, small * len(author) * 1.02)
        out.append(BalloonPlacement(slot=slot, line_index=-2, beat_id="", text=author, balloon="none", speaker="",
                                    x_mm=round(title_box[0] - small * 1.4, 2), y_mm=round(title_box[1] + title_box[3] - height, 2),
                                    w_mm=round(small * 1.1, 2), h_mm=round(height, 2), tail=None,
                                    style={**{k: v for k, v in style.items() if k == "font"}, "size_mm": small}))
    return out


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
        style = {**(p.style or {}), **({"speaker_id": p.speaker_id} if p.speaker_id else {})}
        if style:
            op["style"] = style
        ops.append(op)
    return ops


def _find_spot(rect: Box, size: tuple[float, float], placed: list[Box], figs: list[Figure],
               speaker_id: str | None = None, near: tuple[float, float] | None = None) -> tuple[Box, bool] | None:
    """The best free place for a balloon in the panel: off the faces and bodies, after the previous balloon in
    reading order, and next to its speaker's head (above or beside it, on the speaker's side, never nearer another
    character). Without a known speaker, the top right as manga places them. `near`: a sound effect's source: the
    letters sit on the art as close to it as they can (off the faces)."""
    x0, y0, w, h = rect
    bw, bh = size
    left, top = x0 + MARGIN_MM, y0 + MARGIN_MM
    right, bottom = x0 + w - MARGIN_MM, y0 + h - MARGIN_MM
    if bw > right - left or bh > bottom - top:
        return None
    prev = placed[-1] if placed else None
    speaker = next((f for f in figs if speaker_id and f.char_id == speaker_id), None)
    others = [f for f in figs if f is not speaker]
    best: tuple[float, Box, bool] | None = None
    y = top
    while y + bh <= bottom + 1e-6:
        x = right - bw
        while x >= left - 1e-6:
            box = (x, y, bw, bh)
            if near is not None:
                if not any(_overlap(_grow(box, GAP_MM), other) > 0 for other in placed):
                    face = sum(_overlap(box, f.head) for f in figs)
                    cx, cy = x + bw / 2, y + bh / 2
                    cost = 20000.0 * face / (bw * bh) + ((cx - near[0]) ** 2 + (cy - near[1]) ** 2) ** 0.5
                    if best is None or cost < best[0]:
                        best = (cost, box, face > 0)
            elif not any(_overlap(_grow(box, GAP_MM), other) > 0 for other in placed) and _after(box, prev):
                face = sum(_overlap(box, f.head) for f in figs)
                body = sum(_overlap(box, f.body) for f in figs)
                cost = 20000.0 * face / (bw * bh) + 3.0 * body / (bw * bh)
                if speaker is not None:
                    to_speaker = _gap(box, speaker.head)
                    cx = x + bw / 2
                    hx = speaker.head[0] + speaker.head[2] / 2
                    cost += 2.0 * to_speaker + 0.25 * abs(cx - hx) + 0.3 * max(0.0, y + bh / 2 - (speaker.head[1] + speaker.head[3]))
                    cost += 8.0 * max(0.0, TAIL_ROOM_MM - to_speaker)  # (room for the tail between balloon and face)
                    if any(_gap(box, f.head) + 1.0 < to_speaker for f in others):
                        cost += 60.0  # (nearer someone else: it would read as their line)
                else:
                    cost += (right - (x + bw)) + 1.2 * (y - top)
                if best is None or cost < best[0]:
                    best = (cost, box, face > 0)
            x -= STEP_MM
        y += STEP_MM
    if best is None:
        return None
    return best[1], best[2]


def _gap(a: Box, b: Box) -> float:
    """The distance between two boxes (0 when they touch or overlap)."""
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    dx = max(0.0, bx - (ax + aw), ax - (bx + bw))
    dy = max(0.0, by - (ay + ah), ay - (by + bh))
    return (dx * dx + dy * dy) ** 0.5


def _after(box: Box, prev: Box | None) -> bool:
    """Japanese reading order: the next balloon sits to the left of, or below, the previous one."""
    if prev is None:
        return True
    x, y, w, h = box
    px, py, pw, ph = prev
    return x + w <= px + pw * 0.5 or y >= py + ph * 0.5


def tail_to(box: Box, head: Box, balloon: str = "speech") -> tuple[float, float] | None:
    """Where a balloon's tail ends: toward the speaker's mouth (low in the head box), stopping just short of the
    face so the point never covers it. A thought's bubbles stop a little further off. A balloon right against the
    head gets a short point toward it."""
    x, y, w, h = box
    sx, sy = x + w / 2, y + h / 2
    hx, hy, hw, hh = head
    mx, my = hx + hw / 2, hy + hh * 0.72
    stop = 2.5 if balloon == "thought" else 1.2
    grown = (hx - stop, hy - stop, hw + 2 * stop, hh + 2 * stop)
    dx, dy = mx - sx, my - sy
    dist = (dx * dx + dy * dy) ** 0.5
    if dist < 1e-6:
        return None
    # (the first point of the line from the balloon's middle that enters the head box, grown by the gap)
    enter = 1.0
    for t in [i / 200 for i in range(201)]:
        px, py = sx + dx * t, sy + dy * t
        if grown[0] <= px <= grown[0] + grown[2] and grown[1] <= py <= grown[1] + grown[3]:
            enter = t
            break
    tx, ty = sx + dx * enter, sy + dy * enter
    if x <= tx <= x + w and y <= ty <= y + h:  # (the head is right against the balloon: a short point toward it,
        out = min((w / 2) / abs(dx / dist) if dx else 1e9, (h / 2) / abs(dy / dist) if dy else 1e9)  # never onto the face)
        face = next((t * dist for t in [i / 400 for i in range(401)]
                     if hx <= sx + dx * t <= hx + hw and hy <= sy + dy * t <= hy + hh), dist)
        reach = max(out + 0.3, min(out + 2.0, face - 0.5))
        return (sx + dx / dist * reach, sy + dy / dist * reach)
    reach = ((tx - sx) ** 2 + (ty - sy) ** 2) ** 0.5
    longest = max(w, h) / 2 + TAIL_MAX_MM
    if reach > longest:
        tx, ty = sx + dx / dist * longest, sy + dy / dist * longest
    return (tx, ty)


def _tail(box: Box, balloon: str, speaker_id: str | None, figs: list[Figure]) -> tuple[float, float] | None:
    if balloon in ("narration", "sfx") or not speaker_id:
        return None
    fig = next((f for f in figs if f.char_id == speaker_id), None)
    if fig is None:
        return None
    return tail_to(box, fig.head, balloon)


def _grow(box: Box, by: float) -> Box:
    x, y, w, h = box
    return (x - by, y - by, w + 2 * by, h + 2 * by)


def _overlap(a: Box, b: Box) -> float:
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    dx = min(ax + aw, bx + bw) - max(ax, bx)
    dy = min(ay + ah, by + bh) - max(ay, by)
    return dx * dy if dx > 0 and dy > 0 else 0.0
