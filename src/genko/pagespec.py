"""Changing a book's paper: the size presets by name, and moving everything on each page from the old
basic frame (基本枠) onto the new one, so a book started on the wrong paper can be put right.

Positions follow the basic frame (a stretch in x and y); sizes of lines, balloons and figures scale by the
smaller of the two, so text and pen widths keep their proportions.
"""

from __future__ import annotations

import dataclasses
import io

from genko.models import PAPER_PRESETS, Episode, PageSpec, Rect

STUDIO_NAMES = {"commercial-b4": "b4", "doujin-b5": "b5", "doujin-a5": "a5", "a4-mono": "a4"}


def spec_from(op: dict, current: PageSpec) -> PageSpec:
    """A PageSpec from a preset name or numbers (unset numbers keep the current ones)."""
    preset = op.get("preset")
    if preset:
        key = STUDIO_NAMES.get(str(preset), str(preset))
        if key not in PAPER_PRESETS:
            raise ValueError(f"preset must be one of {', '.join(PAPER_PRESETS)}")
        spec = PAPER_PRESETS[key][1]()
        if op.get("dpi"):
            spec = dataclasses.replace(spec, dpi=int(op["dpi"]))
        return spec
    paper = op.get("paper") or [current.width_mm, current.height_mm]
    trim = op.get("trim") or list(current.trim_size())
    margins = current.margins()
    given = op.get("margins")
    if isinstance(given, dict):
        margins = {**margins, **{k: float(v) for k, v in given.items()}}
    elif given:
        margins = dict(zip(("top", "bottom", "inner", "outer"), (float(v) for v in given)))
    return PageSpec.custom(float(paper[0]), float(paper[1]), float(trim[0]), float(trim[1]),
                           float(op.get("bleed_mm", current.bleed_mm)), margins["top"], margins["bottom"], margins["inner"],
                           margins["outer"], int(op.get("dpi") or current.dpi), current.expression)


class _Map:
    """Old basic frame → new basic frame."""

    def __init__(self, old: Rect, new: Rect) -> None:
        self.old, self.new = old, new
        self.sx = new.width / old.width if old.width else 1.0
        self.sy = new.height / old.height if old.height else 1.0
        self.s = min(self.sx, self.sy)

    def pt(self, x: float, y: float) -> tuple[float, float]:
        return (round(self.new.x + (float(x) - self.old.x) * self.sx, 3), round(self.new.y + (float(y) - self.old.y) * self.sy, 3))

    def box(self, x: float, y: float, w: float, h: float) -> tuple[float, float, float, float]:
        nx, ny = self.pt(x, y)
        return nx, ny, round(float(w) * self.sx, 3), round(float(h) * self.sy, 3)

    def centred(self, x: float, y: float, w: float, h: float) -> tuple[float, float, float, float]:
        """A box that keeps its shape: the middle follows the frame, the size scales by the smaller factor."""
        cx, cy = self.pt(float(x) + float(w) / 2, float(y) + float(h) / 2)
        nw, nh = float(w) * self.s, float(h) * self.s
        return round(cx - nw / 2, 3), round(cy - nh / 2, 3), round(nw, 3), round(nh, 3)


def _frames(frame, m: _Map) -> None:
    frame.rect = Rect(*m.box(frame.rect.x, frame.rect.y, frame.rect.width, frame.rect.height))
    if frame.poly:
        frame.poly = [list(m.pt(x, y)) for x, y in frame.poly]
    for region in (frame.panel or {}).get("regions", []) or []:
        if region.get("rect_mm"):
            region["rect_mm"] = list(m.box(*region["rect_mm"]))
    for child in frame.children:
        _frames(child, m)


def _raster(png: bytes, new_spec: PageSpec, old_frame: Rect, new_frame: Rect) -> bytes:
    """A whole-page paint layer, moved and resized like everything else."""
    from PIL import Image

    from genko.raster import WORKING_DPI
    from genko.render import mm_to_px

    image = Image.open(io.BytesIO(png)).convert("RGBA")
    k = WORKING_DPI / 25.4
    size = (mm_to_px(new_spec.width_mm, WORKING_DPI), mm_to_px(new_spec.height_mm, WORKING_DPI))
    sx = new_frame.width / old_frame.width
    sy = new_frame.height / old_frame.height
    # output pixel (u, v) takes input ((u/k - nx)/sx + ox) * k, … (PIL maps output → input)
    coeffs = (1 / sx, 0, (old_frame.x - new_frame.x / sx) * k, 0, 1 / sy, (old_frame.y - new_frame.y / sy) * k)
    out = image.transform(size, Image.Transform.AFFINE, coeffs, resample=Image.Resampling.BILINEAR)
    buf = io.BytesIO()
    out.save(buf, format="PNG")
    return buf.getvalue()


def relayout(episode: Episode, new_spec: PageSpec, move: bool = True) -> None:
    """Put the book on new paper; with `move`, everything on each page follows the basic frame."""
    olds = {page.index: page.inner_rect_mm(episode.start_side) for page in episode.pages}
    episode.spec = new_spec
    for page in episode.pages:
        page.spec = new_spec
    if not move:
        return
    for page in episode.pages:
        m = _Map(olds[page.index], page.inner_rect_mm(episode.start_side))
        for frame in page.frames:
            _frames(frame, m)
        for layer in page.layers:
            for stroke in layer.strokes:
                stroke.points = [m.pt(x, y) for x, y in stroke.points]
                stroke.width_mm = round(stroke.width_mm * m.s, 4)
            for patch in getattr(layer, "patches", None) or []:
                patch["box"] = list(m.box(*patch["box"]))
            if layer.region:
                layer.region = [m.pt(x, y) for x, y in layer.region]
            if layer.placement_mm is not None:
                layer.placement_mm = Rect(*m.box(layer.placement_mm.x, layer.placement_mm.y, layer.placement_mm.width,
                                                  layer.placement_mm.height))
            if layer.raster_png:
                layer.raster_png = _raster(layer.raster_png, new_spec, m.old, m.new)
        for line in episode.story_for_page(page.index):
            if line.x_mm or line.y_mm:
                line.x_mm, line.y_mm, line.w_mm, line.h_mm = m.centred(line.x_mm, line.y_mm, line.w_mm, line.h_mm)
            if line.tail:
                line.tail = m.pt(*line.tail)
            for tail in line.tails or []:
                if tail.get("to"):
                    tail["to"] = list(m.pt(*tail["to"]))
                if tail.get("via"):
                    tail["via"] = list(m.pt(*tail["via"]))
            if line.path:
                line.path = [m.pt(x, y) for x, y in line.path]
        for ruler in page.rulers:
            ruler["points"] = [list(m.pt(x, y)) for x, y in ruler.get("points") or []]
        if page.ruler and page.ruler.get("points"):
            page.ruler["points"] = [m.pt(x, y) for x, y in page.ruler["points"]]
        for prim in page.prims:
            pos = list(prim.get("pos") or [0, 0, 0]) + [0, 0, 0]
            prim["pos"] = [*m.pt(pos[0], pos[1]), pos[2]]
            if prim.get("size"):
                prim["size"] = [round(float(v) * m.s, 3) for v in prim["size"]]
        for effect in page.effects:
            params = effect.get("params") or {}
            if params.get("center"):
                params["center"] = list(m.pt(*params["center"]))
            if params.get("inner"):
                params["inner"] = [round(float(v) * m.s, 3) for v in params["inner"]]
