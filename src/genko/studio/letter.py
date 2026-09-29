"""Lettering v1: size vertical balloons and place them inside their panels.

Sizes follow the renderer (render._draw_balloon): the em is capped at 5 mm,
text columns run right to left and the balloon adds a quarter-em pad. The
result becomes add_line ops with explicit coordinates, so a replay never
depends on fonts.
"""

from __future__ import annotations

from dataclasses import dataclass, field, replace

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
TAIL_PAST_MM = (3.0, 8.0)  # (a tail points at its speaker: short, at most half the balloon's narrow radius past its edge)
OVER_MM = 6.0  # (a balloon may run over its panel's border this far, into the gutter or the margin, as manga does)
JOINED = ("speech", "rounded", "thought", "whisper")  # (one speaker's lines in a row: two balloons run together)


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
    wrap: str = "vertical"


ELLIPSE_KINDS = ("speech", "thought", "shout", "whisper")
LEADING = 0.4  # (columns this many letters apart: as the renderer sets them)
SFX_EM_MM = 12.0  # the renderer's largest SFX glyph
SPIKED = {"shout": 0.26}
# (when the bible says nothing: a shout is bigger and bold, a whisper smaller; fonts stay the book's own)
DEFAULT_LOOKS: dict[str, dict] = {"shout": {"scale": 1.3, "weight": "bold"}, "whisper": {"scale": 0.8},
                                   "aside": {"font": "hand", "scale": 0.8, "weight": "bold"}}  # (呟き: hand-lettered, small)
ASIDE_TILT = 6.0  # (degrees a 呟き leans, one way then the other)
TAG_EM_MM = 3.2  # (a 名札's letters)
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
        # the ellipse around the letters' own shape (columns from the top, uneven at their feet), a third of a
        # letter to spare (balloons.FIT_PAD); a spiked edge's valleys cut in, so the outline is that much bigger
        from genko import balloons

        rects = []
        for i, col in enumerate(cols):  # (the first column on the right)
            x1 = text_w - i * em * (1 + LEADING)
            rects.append((x1 - em, 0.0, x1, em * max(1, len(col))))
        st = {"spike_depth": SPIKED.get(balloon)} if balloon in SPIKED else {}
        w, h, _dx, _dy = balloons.ellipse_around(balloons.block_points(rects, em * balloons.FIT_PAD), balloons.hug_power(balloon, st))
        keep = balloons._hug_keep(balloon, st)
        return (w / keep, h / keep)
    pad = 0.0 if balloon in ("none", "aside") else em / 4
    if balloon in ("dotted_box", "tone_box", "fancy_box"):  # (as balloons._inner: the frame keeps the words further in)
        pad *= 3
    if balloon == "electric":  # (its zigzag edge leaves the words about three quarters of the box)
        return ((text_w + 2 * pad) / 0.76, (text_h + 2 * pad) / 0.76)
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
    rects = list(layout.leaf_rects_mm.values())
    page = (min(r[0] for r in rects), min(r[1] for r in rects), max(r[0] + r[2] for r in rects),
            max(r[1] + r[3] for r in rects)) if rects else None
    on_page: list[Box] = []  # (every balloon so far, so one running over its border never meets the next panel's)
    for pi, panel in enumerate(plan.get("panels", [])):
        rect = layout.leaf_rects_mm.get(panel.get("slot"))
        if rect is None:
            continue
        figs = figures(panel, rect)
        placed: list[Box] = []
        neighbours = [_grow(r, -2.0) for slot, r in layout.leaf_rects_mm.items() if slot != panel.get("slot")]
        room = Room(page, neighbours, on_page)
        if panel.get("slot") == title_slot:
            for item in _title_placements(panel["slot"], rect, bible, kinds.get("title", {})):
                placements.append(item)
                placed.append((item.x_mm, item.y_mm, item.w_mm, item.h_mm))
        for li, line in enumerate(panel.get("lines", [])):
            path = f"/panels/{pi}/lines/{li}"
            balloon = line.get("balloon", "speech")
            style = look_style(kinds.get(balloon, {}), balloon)
            if balloon == "aside":  # (呟き: no balloon, hand-lettered, small, leaning one way then the other)
                style = {**style, "rotate_deg": ASIDE_TILT if li % 2 == 0 else -ASIDE_TILT}
            em = float(style.get("size_mm") or EM_MM)
            size = measure(line.get("breaks", []), balloon, em)
            speaker_id = speakers.get(line.get("beat_id"))
            near = None
            where = panel.get("sfx_at")
            if balloon == "sfx" and isinstance(where, (list, tuple)) and len(where) == 2:  # (the sound's source, 0..1)
                near = (rect[0] + rect[2] * min(1.0, max(0.0, float(where[0]))), rect[1] + rect[3] * min(1.0, max(0.0, float(where[1]))))
            prev = placements[-1] if placements and placements[-1].slot == panel.get("slot") else None
            group = None
            spot = None
            if (prev is not None and speaker_id and prev.speaker_id == speaker_id and balloon in JOINED
                    and prev.balloon == balloon):  # (the same person again: the second balloon runs out of the first)
                spot = _joined_spot((prev.x_mm, prev.y_mm, prev.w_mm, prev.h_mm), size, rect, placed[:-1], figs, room)
                if spot is not None:
                    group = (prev.style or {}).get("group") or f"said{pi}.{prev.line_index}"
                    placements[-1] = replace(prev, style={**(prev.style or {}), "group": group})
            if spot is None:
                spot = _find_spot(rect, size, placed, figs, speaker_id, near, room)
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
            if covers_face and balloon not in ("sfx", "none"):  # (a sound or bare text over a face is the drawing's business)
                issues.append(warning("balloon_covers_face", path, f"コマ {panel.get('slot')} で台詞が顔にかかる", "台詞を減らすか、人物の pos を変える"))
            placed.append(box)
            on_page.append(box)
            if group:
                style = {**(style or {}), "group": group}
            placements.append(
                BalloonPlacement(
                    slot=panel["slot"],
                    line_index=li,
                    beat_id=line.get("beat_id", ""),
                    text="\n".join(line.get("breaks", [])),
                    balloon="none" if balloon == "aside" else balloon,
                    speaker=names.get(speaker_id, "") if speaker_id else "",
                    x_mm=round(box[0], 2),
                    y_mm=round(box[1], 2),
                    w_mm=round(box[2], 2),
                    h_mm=round(box[3], 2),
                    tail=None if group else _tail(box, line.get("balloon", "speech"), speaker_id, figs),
                    speaker_id=speaker_id or "",
                    style=style or None,
                )
            )
        for ci, char in enumerate(panel.get("characters", []) or []):  # (名札: a small label by the character)
            tag = str(char.get("tag") or "").strip()
            fig = next((f for f in figs if f.char_id == char.get("id")), None)
            if not tag or fig is None:
                continue
            h, w = measure([tag], "rounded", TAG_EM_MM)  # (set across: the column's size turned)
            near = (fig.head[0] + fig.head[2] / 2, fig.body[1] + fig.body[3] * 0.55)
            spot = _find_spot(rect, (w, h), placed, figs, None, near)
            if spot is None:
                issues.append(warning("tag_overflow", f"/panels/{pi}/characters/{ci}/tag", f"コマ {panel.get('slot')} に名札が入らない",
                                      "名札を短くするか、コマを大きくする"))
                continue
            box = spot[0]
            placed.append(box)
            on_page.append(box)
            placements.append(BalloonPlacement(slot=panel["slot"], line_index=-10 - ci, beat_id="", text=tag, balloon="rounded",
                                               speaker="", x_mm=round(box[0], 2), y_mm=round(box[1], 2), w_mm=round(box[2], 2),
                                               h_mm=round(box[3], 2), tail=None,
                                               style={"font": "gothic", "size_mm": TAG_EM_MM}, wrap="horizontal"))
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
            "wrap": p.wrap,
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


@dataclass(frozen=True)
class Room:
    """Where a balloon may go beyond its panel: `page` (the panels' outer edge), never into `neighbours` (the other
    panels, 2 mm in) nor onto `taken` (balloons already placed on the page)."""
    page: tuple[float, float, float, float] | None = None
    neighbours: list = field(default_factory=list)
    taken: list = field(default_factory=list)

    def allows(self, box: Box) -> bool:
        if any(_overlap(box, n) > 0 for n in self.neighbours):
            return False
        return not any(_overlap(_grow(box, GAP_MM), t) > 0 for t in self.taken)


def _over(box: Box, rect: Box) -> float:
    """How far (mm, all sides together) a box runs past the panel's inner margin."""
    x, y, w, h = box
    x0, y0, rw, rh = rect
    return (max(0.0, x0 + MARGIN_MM - x) + max(0.0, x + w - (x0 + rw - MARGIN_MM))
            + max(0.0, y0 + MARGIN_MM - y) + max(0.0, y + h - (y0 + rh - MARGIN_MM)))


def _find_spot(rect: Box, size: tuple[float, float], placed: list[Box], figs: list[Figure],
               speaker_id: str | None = None, near: tuple[float, float] | None = None,
               room: Room | None = None) -> tuple[Box, bool] | None:
    """The best free place for a balloon in the panel: off the faces and bodies, after the previous balloon in
    reading order, and next to its speaker's head (above or beside it, on the speaker's side, never nearer another
    character). Without a known speaker, the top right as manga places them. `near`: a sound effect's source: the
    letters sit on the art as close to it as they can (off the faces). With `room`, a balloon may run over the
    panel's border (into the gutter or the margin, never into another panel) where that brings it to its speaker."""
    x0, y0, w, h = rect
    bw, bh = size
    left, top = x0 + MARGIN_MM, y0 + MARGIN_MM
    right, bottom = x0 + w - MARGIN_MM, y0 + h - MARGIN_MM
    inner_right, inner_top = right, top
    if room is not None and near is None:
        ox, oy = min(OVER_MM, bw * 0.3), min(OVER_MM, bh * 0.3)
        left, top, right, bottom = x0 - ox, y0 - oy, x0 + w + ox, y0 + h + oy
        if room.page is not None:  # (at most a little past the panels' outer edge, into the page's margin)
            px0, py0, px1, py1 = room.page
            left, top = max(left, px0 - 4.0), max(top, py0 - 4.0)
            right, bottom = min(right, px1 + 4.0), min(bottom, py1 + 4.0)
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
            elif (not any(_overlap(_grow(box, GAP_MM), other) > 0 for other in placed) and _after(box, prev)
                  and (room is None or room.allows(box)) and _centre_in(box, rect)):
                face = sum(_overlap(box, f.head) for f in figs)
                body = sum(_overlap(box, f.body) for f in figs)
                cost = 20000.0 * face / (bw * bh) + 3.0 * body / (bw * bh) + 0.6 * _over(box, rect)
                if speaker is not None:
                    to_speaker = _gap(box, speaker.head)
                    cx = x + bw / 2
                    hx = speaker.head[0] + speaker.head[2] / 2
                    cost += 2.0 * to_speaker + 0.25 * abs(cx - hx) + 0.3 * max(0.0, y + bh / 2 - (speaker.head[1] + speaker.head[3]))
                    cost += 8.0 * max(0.0, TAIL_ROOM_MM - to_speaker)  # (room for the tail between balloon and face)
                    if any(_gap(box, f.head) + 1.0 < to_speaker for f in others):
                        cost += 60.0  # (nearer someone else: it would read as their line)
                else:
                    cost += abs(inner_right - (x + bw)) + 1.2 * abs(y - inner_top)
                if best is None or cost < best[0]:
                    best = (cost, box, face > 0)
            x -= STEP_MM
        y += STEP_MM
    if best is None:
        return None
    return best[1], best[2]


def _centre_in(box: Box, rect: Box) -> bool:
    """The balloon's middle is in its panel (running over the border, it still belongs to the panel)."""
    cx, cy = box[0] + box[2] / 2, box[1] + box[3] / 2
    return rect[0] <= cx <= rect[0] + rect[2] and rect[1] <= cy <= rect[1] + rect[3]


def _joined_spot(prev: Box, size: tuple[float, float], rect: Box, placed: list[Box], figs: list[Figure],
                 room: Room) -> tuple[Box, bool] | None:
    """A second balloon for the same speaker, run out of the first to its left and a little lower (read next), the
    two overlapping by about a fifth, so they draw as one outline; None when there is no room for it."""
    px, py, pw, ph = prev
    bw, bh = size
    lap = 0.22 * min(pw, bw)
    x = px - bw + lap
    best = None
    for share in (0.3, 0.2, 0.4, 0.1, 0.5, 0.0, 0.6):
        box = (x, py + ph * share, bw, bh)
        if not _centre_in(box, rect) or _over(box, rect) > OVER_MM * 1.5:
            continue
        if any(_overlap(_grow(box, GAP_MM), other) > 0 for other in placed):
            continue
        if not Room(room.page, room.neighbours, room.taken[:-1]).allows(box):  # (the first balloon is the last placed)
            continue
        if any(_overlap(box, f.head) > 0 for f in figs):
            continue
        best = box
        break
    return (best, False) if best is not None else None


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
    ux, uy = dx / dist, dy / dist
    edge = 1 / max(1e-6, ((ux / (w / 2)) ** 2 + (uy / (h / 2)) ** 2) ** 0.5)  # (the ellipse's edge that way)
    past = max(TAIL_PAST_MM[0], min(TAIL_PAST_MM[1], min(w, h) / 4))
    longest = min(edge + past, max(w, h) / 2 + TAIL_MAX_MM)
    if reach > longest:  # (it points at the mouth and stops well short: the reader's eye does the rest)
        tx, ty = sx + ux * longest, sy + uy * longest
    return (tx, ty)


def _tail(box: Box, balloon: str, speaker_id: str | None, figs: list[Figure]) -> tuple[float, float] | None:
    if balloon in ("narration", "sfx", "aside", "none", "dotted_box", "tone_box", "fancy_box") or not speaker_id:
        return None
    fig = next((f for f in figs if f.char_id == speaker_id), None)
    if fig is None:
        return None
    if balloon not in ("thought", "shout", "electric") and obvious(box, fig, figs):
        return None
    return tail_to(box, fig.head, balloon)


def obvious(box: Box, speaker: Figure, figs: list[Figure]) -> bool:
    """Whether a balloon needs no tail: its speaker is the only one in the panel, or it sits right by the
    speaker's head with everyone else well away (manga leaves about half its balloons without one)."""
    others = [f for f in figs if f is not speaker]
    if not others:
        return True
    near = _gap(box, speaker.head)
    return near <= 3.0 and all(_gap(box, f.head) >= max(12.0, near + 10.0) for f in others)


def _grow(box: Box, by: float) -> Box:
    x, y, w, h = box
    return (x - by, y - by, w + 2 * by, h + 2 * by)


def _overlap(a: Box, b: Box) -> float:
    ax, ay, aw, ah = a
    bx, by, bw, bh = b
    dx = min(ax + aw, bx + bw) - max(ax, bx)
    dy = min(ay + ah, by + bh) - max(ay, by)
    return dx * dy if dx > 0 and dy > 0 else 0.0
