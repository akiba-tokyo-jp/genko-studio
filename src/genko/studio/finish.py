"""finish_page: the deterministic finishing pass after the art is approved.

1. Balloons that cover a reported face move to the best free spot in their
   panel (same search as the name lettering, with the reported regions as
   figures). Reading order inside the panel is kept.
2. `panel.fx` words (studio/fxwords.py) become effects clipped to the panel (集中線, 流線, フラッシュ…), manga
   marks next to the panel's first reported face (汗, 怒り…) or rain streaks over it, once each. Words drawn by
   the image tool (水しぶき…) and unknown words are noted, not drawn.
3. The page advances to finish.
"""

from __future__ import annotations

from genko.models import Episode, Page
from genko.studio import fxwords
from genko.studio.blocking import Figure
from genko.studio.letter import _find_spot, _gap, _overlap, tail_to

FX_LAYER = "効果（仕上げ）"
FACE_KINDS = ("face", "head")
BODY_KINDS = ("person", "body")


def region_figures(panel: dict) -> list[Figure]:
    figures = []
    regions = [r for r in panel.get("regions", []) if isinstance(r.get("rect_mm"), (list, tuple)) and len(r["rect_mm"]) == 4]
    bodies = [r for r in regions if r.get("kind") in BODY_KINDS]
    for face in (r for r in regions if r.get("kind") in FACE_KINDS):
        head = tuple(float(v) for v in face["rect_mm"])
        body = next((tuple(float(v) for v in b["rect_mm"]) for b in bodies if b.get("char") and b.get("char") == face.get("char")), head)
        figures.append(Figure(str(face.get("char") or ""), head, body))
    for body in bodies:
        if not any(f.char_id and f.char_id == body.get("char") for f in figures):
            box = tuple(float(v) for v in body["rect_mm"])
            figures.append(Figure(str(body.get("char") or ""), (box[0], box[1], box[2], min(box[3], box[2])), box))
    return figures


def _fx_layer(page: Page, ops: list[dict]) -> tuple[list[dict], str]:
    """The page's pen layer for finishing marks and rain (made once, on top)."""
    layer_id = f"fx-{page.id}"[:40]
    if any(layer.id == layer_id for layer in page.layers) or any(op.get("id") == layer_id for op in ops):
        return [], layer_id
    return [{"op": "add_layer", "page": page.index, "name": FX_LAYER, "kind": "pen", "id": layer_id}], layer_id


def _mark_spot(frame, figs: list[Figure]) -> tuple[float, float, str]:
    """Beside the first reported face (above its outer corner), inside the panel; the panel's upper part without."""
    r = frame.rect
    if figs:
        hx, hy, hw, hh = figs[0].head
        x = hx + hw + 5 if hx + hw + 10 <= r.x + r.width else hx - 5
        y = max(r.y + 6, hy + 2)
        return round(min(max(x, r.x + 6), r.x + r.width - 6), 2), round(min(y, r.y + r.height - 6), 2), ""
    return (round(r.x + r.width * 0.25, 2), round(r.y + r.height * 0.25, 2),
            "顔の位置が報告されていないので、コマの左上に置いた。report_regions の後に仕上げると顔の横に置く")


def _rain(page: Page, frame, layer_id: str) -> list[dict]:
    """Thin slanting streaks over the panel, the same for the same panel every time."""
    import random

    r = frame.rect
    rng = random.Random(frame.id)
    count = max(12, min(220, int(r.width * r.height / 90)))
    ops = []
    for _ in range(count):
        length = rng.uniform(5.0, 13.0)
        x = rng.uniform(r.x - 4, r.x + r.width)
        y = rng.uniform(r.y - 6, r.y + r.height)
        dx, dy = length * 0.26, length  # (about 15° from vertical, falling to the right)
        ops.append({"op": "add_stroke", "page": page.index, "layer_id": layer_id, "points": [[round(x, 2), round(y, 2)],
                    [round(x + dx, 2), round(y + dy, 2)]], "width_mm": round(rng.uniform(0.12, 0.22), 3), "taper": True})
    return ops


def _tailed(line) -> bool:
    """A line whose balloon points at its speaker (not narration, sound or a line with hand-drawn tails)."""
    return (line.balloon or "speech") not in ("narration", "sfx", "none", "box") and not line.tails


AIMED = ("focus", "uni_flash", "white")  # (lines that close in on a point: aimed at the faces, which they leave clear)


def _aim(kind: str, figs: list[Figure]) -> dict:
    """Centre and clear middle of focus lines around the reported faces (all of them), so no line crosses a face."""
    if kind not in AIMED or not figs:
        return {}
    heads = [f.head for f in figs]
    x0 = min(h[0] for h in heads)
    y0 = min(h[1] for h in heads)
    x1 = max(h[0] + h[2] for h in heads)
    y1 = max(h[1] + h[3] for h in heads)
    # (an ellipse through the corners of the faces' box, a little wider: √2 × half the box, and a margin)
    return {"center": [round((x0 + x1) / 2, 2), round((y0 + y1) / 2, 2)],
            "inner": [round((x1 - x0) / 2 * 1.5 + 2, 2), round((y1 - y0) / 2 * 1.5 + 2, 2)]}


def plan(episode: Episode, page: Page) -> tuple[list[dict], list[dict]]:
    """(ops, proposals). Proposals explain each op for the preview."""
    ops: list[dict] = []
    notes: list[dict] = []
    lines = episode.story_for_page(page.index)
    for frame in page.leaf_frames():
        panel = frame.panel or {}
        figs = region_figures(panel)
        in_frame = [line for line in lines if line.frame_id == frame.id]
        if figs and in_frame:
            rect = (frame.rect.x, frame.rect.y, frame.rect.width, frame.rect.height)
            placed: list[tuple] = []
            for line in in_frame:
                box = (line.x_mm, line.y_mm, line.w_mm, line.h_mm)
                who = str((line.style or {}).get("speaker_id") or "")
                speaker = next((f for f in figs if who and f.char_id == who), None)
                covers = any(_overlap(box, f.head) > 0 for f in figs)
                misread = speaker is not None and any(_gap(box, f.head) + 1.0 < _gap(box, speaker.head)
                                                      for f in figs if f is not speaker)
                if covers or misread:
                    spot = _find_spot(rect, (line.w_mm, line.h_mm), placed, figs, who or None)
                    if spot is not None and not spot[1]:
                        new = spot[0]
                        move = {"op": "move_line", "id": line.id, "x_mm": round(new[0], 2), "y_mm": round(new[1], 2)}
                        tip = tail_to(new, speaker.head, line.balloon or "speech") if speaker is not None and _tailed(line) else None
                        if tip is not None:
                            move["tail"] = [round(tip[0], 2), round(tip[1], 2)]
                        ops.append(move)
                        notes.append({"kind": "move_line", "frame_id": frame.id, "line": line.text[:20],
                                      "from": [round(line.x_mm, 1), round(line.y_mm, 1)], "to": [round(new[0], 1), round(new[1], 1)],
                                      "why": "顔にかかっていた" if covers else "話していない人の近くにあった"})
                        placed.append(new)
                        continue
                    if covers:
                        notes.append({"kind": "face_covered", "frame_id": frame.id, "line": line.text[:20],
                                      "why": "顔を避けて置ける場所が無い。台詞を減らすか人の手で動かす"})
                elif speaker is not None and _tailed(line):  # (the tail points at where the speaker really is)
                    tip = tail_to(box, speaker.head, line.balloon or "speech")
                    if tip is not None and (line.tail is None or abs(tip[0] - line.tail[0]) + abs(tip[1] - line.tail[1]) > 1.0):
                        ops.append({"op": "move_line", "id": line.id, "tail": [round(tip[0], 2), round(tip[1], 2)]})
                        notes.append({"kind": "aim_tail", "frame_id": frame.id, "line": line.text[:20]})
                placed.append(box)
        existing = {(e.get("kind"), e.get("frame_id")) for e in page.effects}
        done = set(panel.get("fx_done") or [])
        drawn: list[str] = []
        for word in panel.get("fx", []) or []:
            word = str(word).strip()
            found = fxwords.resolve(word)
            if found is None:
                notes.append({"kind": "fx_unknown", "frame_id": frame.id, "word": word,
                              "why": "Genko の知らない効果の言葉。絵の依頼文には入れた。描くなら apply_ops で"})
                continue
            if found["kind"] == "art":
                notes.append({"kind": "fx_in_art", "frame_id": frame.id, "word": word, "why": "画像ツールが描く効果（依頼文に入れた）"})
                continue
            if found["kind"] == "effect":
                kind = found["key"]
                if (kind, frame.id) in existing:
                    continue
                existing.add((kind, frame.id))
                params = _aim(kind, figs)
                ops.append({"op": "add_effect", "page": page.index, "kind": kind, "frame_id": frame.id, "params": params})
                note = {"kind": "add_effect", "frame_id": frame.id, "effect": kind}
                if kind in AIMED and not params:
                    note["why"] = "顔の位置が報告されていないので、コマの中心に向けた。report_regions で顔を報告すると顔に合わせる"
                notes.append(note)
                continue
            if word in done:
                continue
            layer_ops, layer_id = _fx_layer(page, ops)
            ops.extend(layer_ops)
            if found["kind"] == "mark":
                x, y, why = _mark_spot(frame, figs)
                ops.append({"op": "stamp_material", "page": page.index, "material_id": f"mark-{found['key']}", "layer_id": layer_id,
                            "x_mm": x, "y_mm": y})
                notes.append({"kind": "add_mark", "frame_id": frame.id, "mark": found["key"], **({"why": why} if why else {})})
            else:  # rain
                ops.extend(_rain(page, frame, layer_id))
                notes.append({"kind": "add_rain", "frame_id": frame.id})
            drawn.append(word)
        if drawn:
            ops.append({"op": "set_panel", "page": page.index, "frame_id": frame.id, "set": {"fx_done": sorted(done | set(drawn))}})
    if page.stage != "finish":
        ops.append({"op": "advance", "page": page.index, "to": "finish"})
    return ops, notes
