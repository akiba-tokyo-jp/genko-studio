"""Where a placed image lands on the page, and what clips it.

The image is resampled from its source at render time (never stretched from a
page-sized copy). `clip_to`: "frame" clips to the panel, "bleed" extends the
panel's outer edges to the paper edge (for panels marked bleed), "none" clips
to the page.
"""

from __future__ import annotations

from genko.models import Frame, Page, Rect

EDGE_EPS_MM = 0.5


def outer_edges(page: Page, frame: Frame) -> dict[str, bool]:
    """Which sides of the panel touch the live area's edge (so they may bleed)."""
    inner = page.inner_rect_mm()
    r = frame.rect
    return {
        "left": abs(r.x - inner.x) < EDGE_EPS_MM,
        "top": abs(r.y - inner.y) < EDGE_EPS_MM,
        "right": abs((r.x + r.width) - (inner.x + inner.width)) < EDGE_EPS_MM,
        "bottom": abs((r.y + r.height) - (inner.y + inner.height)) < EDGE_EPS_MM,
    }


def bleed_poly(page: Page, frame: Frame | None) -> list | None:
    """A slanted or free-form bleed panel run out to the bleed: its corners on the live area's edge move out to the
    bleed's edge (so the slanted sides keep their slant and only the outer sides go off the paper). None for others."""
    if frame is None or not getattr(frame, "bleed", False) or not getattr(frame, "poly", None):
        return None
    inner, bleed = page.inner_rect_mm(), page.bleed_rect_mm()
    out = []
    for point in frame.poly:
        x, y = float(point[0]), float(point[1])
        if abs(x - inner.x) < EDGE_EPS_MM:
            x = bleed.x
        elif abs(x - (inner.x + inner.width)) < EDGE_EPS_MM:
            x = bleed.x + bleed.width
        if abs(y - inner.y) < EDGE_EPS_MM:
            y = bleed.y
        elif abs(y - (inner.y + inner.height)) < EDGE_EPS_MM:
            y = bleed.y + bleed.height
        out.append([x, y])
    return out


def on_bleed_edge(page: Page, a, b) -> bool:
    """Whether a side of a bleed polygon lies along the bleed's edge (it is cut off: no border there)."""
    bleed = page.bleed_rect_mm()
    for fixed, index in ((bleed.x, 0), (bleed.x + bleed.width, 0), (bleed.y, 1), (bleed.y + bleed.height, 1)):
        if abs(a[index] - fixed) < EDGE_EPS_MM and abs(b[index] - fixed) < EDGE_EPS_MM:
            return True
    return False


def in_poly(points, x: float, y: float) -> bool:
    inside = False
    for (x0, y0), (x1, y1) in zip(points, points[1:] + points[:1]):
        if (y0 > y) != (y1 > y) and x < x0 + (y - y0) * (x1 - x0) / (y1 - y0):
            inside = not inside
    return inside


def clip_box(page: Page, frame: Frame | None, clip_to: str) -> Rect:
    full = Rect(0, 0, page.spec.width_mm, page.spec.height_mm)
    if frame is None or clip_to == "none":
        return full
    r = frame.rect
    if clip_to != "bleed":
        return r
    edges = outer_edges(page, frame)
    bleed = page.bleed_rect_mm()  # a bleed panel runs out to the bleed (the part that is cut off)
    x0 = bleed.x if edges["left"] else r.x
    y0 = bleed.y if edges["top"] else r.y
    x1 = bleed.x + bleed.width if edges["right"] else r.x + r.width
    y1 = bleed.y + bleed.height if edges["bottom"] else r.y + r.height
    return Rect(x0, y0, x1 - x0, y1 - y0)


def fit_rect(target: Rect, image_w: int, image_h: int, fit: str = "cover",
             offset_mm: tuple[float, float] = (0.0, 0.0), scale: float = 1.0) -> Rect:
    """The page rect (mm) the whole image occupies so that it covers / fits in `target`."""
    if fit == "stretch" or image_w <= 0 or image_h <= 0:
        w, h = target.width, target.height
    else:
        ratio = image_w / image_h
        by_width = (target.width, target.width / ratio)
        by_height = (target.height * ratio, target.height)
        if fit == "contain":
            w, h = by_width if by_width[1] <= target.height else by_height
        else:  # cover
            w, h = by_width if by_width[1] >= target.height else by_height
    w, h = w * scale, h * scale
    x = target.x + (target.width - w) / 2 + offset_mm[0]
    y = target.y + (target.height - h) / 2 + offset_mm[1]
    return Rect(round(x, 3), round(y, 3), round(w, 3), round(h, 3))


def default_target(page: Page, frame: Frame, clip_to: str, pad_mm: float = 0.0) -> Rect:
    box = clip_box(page, frame, clip_to)
    return Rect(box.x - pad_mm, box.y - pad_mm, box.width + 2 * pad_mm, box.height + 2 * pad_mm)
