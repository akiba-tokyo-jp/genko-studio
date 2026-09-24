"""finish_page: the deterministic finishing pass after the art is approved.

1. Balloons that cover a reported face move to the best free spot in their
   panel (same search as the name lettering, with the reported regions as
   figures). Reading order inside the panel is kept.
2. `panel.fx` words become effects clipped to the panel (集中線 → focus,
   流線 → speed, フラッシュ → white), once.
3. The page advances to finish.
"""

from __future__ import annotations

from genko.models import Episode, Page
from genko.studio.blocking import Figure
from genko.studio.letter import _find_spot, _overlap

FX_KINDS = {
    "集中線": "focus", "focus": "focus", "focus_lines": "focus",
    "流線": "speed", "speed": "speed", "speed_lines": "speed",
    "フラッシュ": "white", "white": "white", "flash": "white",
}
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
                if any(_overlap(box, f.head) > 0 for f in figs):
                    spot = _find_spot(rect, (line.w_mm, line.h_mm), placed, figs)
                    if spot is not None and not spot[1]:
                        new = spot[0]
                        ops.append({"op": "move_line", "id": line.id, "x_mm": round(new[0], 2), "y_mm": round(new[1], 2)})
                        notes.append({"kind": "move_line", "frame_id": frame.id, "line": line.text[:20],
                                      "from": [round(line.x_mm, 1), round(line.y_mm, 1)], "to": [round(new[0], 1), round(new[1], 1)]})
                        placed.append(new)
                        continue
                    notes.append({"kind": "face_covered", "frame_id": frame.id, "line": line.text[:20],
                                  "why": "顔を避けて置ける場所が無い。台詞を減らすか人の手で動かす"})
                placed.append(box)
        existing = {(e.get("kind"), e.get("frame_id")) for e in page.effects}
        for word in panel.get("fx", []) or []:
            kind = FX_KINDS.get(str(word).strip())
            if kind and (kind, frame.id) not in existing:
                existing.add((kind, frame.id))
                ops.append({"op": "add_effect", "page": page.index, "kind": kind, "frame_id": frame.id, "params": {}})
                notes.append({"kind": "add_effect", "frame_id": frame.id, "effect": kind})
    if page.stage != "finish":
        ops.append({"op": "advance", "page": page.index, "to": "finish"})
    return ops, notes
