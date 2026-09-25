"""Panel geometry: shapes (rectangles or polygons), cuts, gutters, and re-laying a panel tree.

A leaf panel is its `rect`, or its `poly` (page mm) when it is slanted or free-form; `rect` is then
the polygon's bounding box, so everything that only needs a box keeps working. A split node keeps
how it was cut in `split`: the cut line in its own box's 0..1 coordinates and the gutter in mm, so a
cut follows the node when a gutter elsewhere moves. Nodes built without `split` (older books, agent
layouts) are stacks along `split_axis`: moving a gutter there trades span between the two neighbours.
"""

from __future__ import annotations

import math

from genko.models import Frame, Rect

Point = tuple[float, float]
EPS = 1e-6


# --- shapes -------------------------------------------------------------------------------------------------


def corners(rect: Rect) -> list[Point]:
    return [(rect.x, rect.y), (rect.x + rect.width, rect.y), (rect.x + rect.width, rect.y + rect.height), (rect.x, rect.y + rect.height)]


def shape(frame: Frame) -> list[Point]:
    poly = getattr(frame, "poly", None)
    return [(float(x), float(y)) for x, y in poly] if poly else corners(frame.rect)


def bbox(points: list[Point]) -> Rect:
    xs, ys = [p[0] for p in points], [p[1] for p in points]
    return Rect(round(min(xs), 3), round(min(ys), 3), round(max(xs) - min(xs), 3), round(max(ys) - min(ys), 3))


def as_rect(points: list[Point]) -> Rect | None:
    """The rectangle these points are, if they are one (axis-aligned)."""
    if len(points) != 4:
        cleaned = _dedupe(points)
        if len(cleaned) != 4:
            return None
        points = cleaned
    box = bbox(points)
    for x, y in points:
        on_x = abs(x - box.x) < 1e-3 or abs(x - (box.x + box.width)) < 1e-3
        on_y = abs(y - box.y) < 1e-3 or abs(y - (box.y + box.height)) < 1e-3
        if not (on_x and on_y):
            return None
    return box


def _dedupe(points: list[Point]) -> list[Point]:
    out: list[Point] = []
    for p in points:
        if not out or math.dist(p, out[-1]) > 1e-4:
            out.append(p)
    if len(out) > 1 and math.dist(out[0], out[-1]) <= 1e-4:
        out.pop()
    return out


def set_shape(frame: Frame, points: list[Point]) -> None:
    rect = as_rect(points)
    if rect is not None:
        frame.rect, frame.poly = rect, None
    else:
        pts = [(round(x, 3), round(y, 3)) for x, y in _dedupe(points)]
        frame.rect, frame.poly = bbox(pts), pts


def contains(frame: Frame, x: float, y: float) -> bool:
    if not getattr(frame, "poly", None):
        return frame.rect.contains(x, y)
    inside = False
    pts = shape(frame)
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / ((yj - yi) or EPS) + xi:
            inside = not inside
        j = i
    return inside


def area(points: list[Point]) -> float:
    return abs(sum(a[0] * b[1] - b[0] * a[1] for a, b in zip(points, points[1:] + points[:1]))) / 2


def centroid(points: list[Point]) -> Point:
    return sum(p[0] for p in points) / len(points), sum(p[1] for p in points) / len(points)


# --- cutting ----------------------------------------------------------------------------------------------------


def clip_half(points: list[Point], p0: Point, p1: Point, keep_left: bool, offset: float) -> list[Point]:
    """Sutherland-Hodgman against the line p0→p1 moved `offset` mm to the kept side."""
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    length = math.hypot(dx, dy) or 1.0
    nx, ny = -dy / length, dx / length  # left normal (in page coordinates, y down)
    sign = 1.0 if keep_left else -1.0

    def side(p: Point) -> float:
        return sign * ((p[0] - p0[0]) * nx + (p[1] - p0[1]) * ny) - offset

    out: list[Point] = []
    for i, cur in enumerate(points):
        prev = points[i - 1]
        sc, sp = side(cur), side(prev)
        if sc >= 0:
            if sp < 0:
                t = sp / (sp - sc)
                out.append((prev[0] + (cur[0] - prev[0]) * t, prev[1] + (cur[1] - prev[1]) * t))
            out.append(cur)
        elif sp >= 0:
            t = sp / (sp - sc)
            out.append((prev[0] + (cur[0] - prev[0]) * t, prev[1] + (cur[1] - prev[1]) * t))
    return _dedupe(out)


def cut(points: list[Point], p0: Point, p1: Point, gutter: float) -> tuple[list[Point], list[Point]]:
    """The two sides of a shape cut along p0→p1 with a gutter, in reading-friendly order: for a mostly
    horizontal cut the upper part first, for a mostly vertical cut the left part first."""
    left = clip_half(points, p0, p1, True, gutter / 2)
    right = clip_half(points, p0, p1, False, gutter / 2)
    if len(left) < 3 or len(right) < 3 or area(left) < 1 or area(right) < 1:
        raise ValueError("the cut does not cross the panel")
    horizontal = abs(p1[0] - p0[0]) >= abs(p1[1] - p0[1])
    ca, cb = centroid(left), centroid(right)
    first_is_left = ca[1] < cb[1] if horizontal else ca[0] < cb[0]
    return (left, right) if first_is_left else (right, left)


def _norm(rect: Rect, p: Point) -> list[float]:
    return [round((p[0] - rect.x) / (rect.width or 1), 6), round((p[1] - rect.y) / (rect.height or 1), 6)]


def _denorm(rect: Rect, q) -> Point:
    return rect.x + float(q[0]) * rect.width, rect.y + float(q[1]) * rect.height


def cut_line(node: Frame) -> tuple[Point, Point, float] | None:
    """The cut of a split node in page mm (p0, p1, gutter), or None for a stack."""
    split = getattr(node, "split", None)
    if not split or "a" not in split:
        return None
    return _denorm(node.rect, split["a"]), _denorm(node.rect, split["b"]), float(split.get("gutter_mm", 4.0))


def cut_frame(node: Frame, p0: Point, p1: Point, gutter: float, new_id) -> tuple[Frame, Frame]:
    """Cut a leaf in two along p0→p1 (page mm); the leaf becomes their parent and keeps the cut."""
    first, second = cut(shape(node), p0, p1, gutter)
    # extend the line to the node's box so the stored cut does not depend on where the drag started
    a, b = _extend(node.rect, p0, p1)
    node.split = {"a": _norm(node.rect, a), "b": _norm(node.rect, b), "gutter_mm": round(float(gutter), 3)}
    horizontal = abs(p1[0] - p0[0]) >= abs(p1[1] - p0[1])
    node.split_axis = "horizontal" if horizontal else "vertical"
    children = []
    for pts in (first, second):
        child = Frame(id=new_id(), rect=bbox(pts))
        set_shape(child, pts)
        children.append(child)
    node.children = children
    return children[0], children[1]


def _extend(rect: Rect, p0: Point, p1: Point) -> tuple[Point, Point]:
    dx, dy = p1[0] - p0[0], p1[1] - p0[1]
    if abs(dx) >= abs(dy):
        k = dy / (dx or EPS)
        x0, x1 = rect.x, rect.x + rect.width
        return (x0, p0[1] + (x0 - p0[0]) * k), (x1, p0[1] + (x1 - p0[0]) * k)
    k = dx / (dy or EPS)
    y0, y1 = rect.y, rect.y + rect.height
    return (p0[0] + (y0 - p0[1]) * k, y0), (p0[0] + (y1 - p0[1]) * k, y1)


def axis_line(node: Frame, axis: str, ratio: float, gutter: float, tilt: float = 0.0) -> tuple[Point, Point]:
    """The cut line split_frame means: `ratio` of the space left after the gutter, optionally tilted
    by `tilt` mm between its two ends."""
    r = node.rect
    if axis == "horizontal":
        y = r.y + (r.height - gutter) * ratio + gutter / 2
        return (r.x, y - tilt / 2), (r.x + r.width, y + tilt / 2)
    x = r.x + (r.width - gutter) * ratio + gutter / 2
    return (x - tilt / 2, r.y), (x + tilt / 2, r.y + r.height)


def remember_split(node: Frame) -> None:
    """Give a two-child axis split (made before cuts were stored) its cut, from its children."""
    if getattr(node, "split", None) or len(node.children) != 2:
        return
    a, b = node.children
    r = node.rect
    if node.split_axis == "horizontal":
        top, bottom = sorted((a, b), key=lambda f: f.rect.y)
        gutter = bottom.rect.y - (top.rect.y + top.rect.height)
        y = top.rect.y + top.rect.height + gutter / 2
        node.split = {"a": _norm(r, (r.x, y)), "b": _norm(r, (r.x + r.width, y)), "gutter_mm": round(gutter, 3)}
    else:
        left, right = sorted((a, b), key=lambda f: f.rect.x)
        gutter = right.rect.x - (left.rect.x + left.rect.width)
        x = left.rect.x + left.rect.width + gutter / 2
        node.split = {"a": _norm(r, (x, r.y)), "b": _norm(r, (x, r.y + r.height)), "gutter_mm": round(gutter, 3)}


# --- re-laying ------------------------------------------------------------------------------------------------


def relayout(node: Frame, points: list[Point] | None = None) -> None:
    """Give `node` a new shape (or keep its own) and lay its children out again inside it."""
    old = Rect(node.rect.x, node.rect.y, node.rect.width, node.rect.height)
    if points is not None:
        if not node.children and getattr(node, "custom", False) and getattr(node, "poly", None):
            points = [_map(old, bbox(points), p) for p in node.poly]  # a free-form panel keeps its form
        set_shape(node, points)
    if not node.children:
        return
    line = cut_line(node)
    if line is not None and len(node.children) == 2:
        p0, p1, gutter = line
        first, second = cut(shape(node), p0, p1, gutter)
        a, b = node.children
        # keep each child on its side (the order of children does not change)
        ca, c1 = centroid(shape(a)), centroid(first)
        c2 = centroid(second)
        if math.dist(ca, c1) > math.dist(ca, c2):
            first, second = second, first
        relayout(a, first)
        relayout(b, second)
        return
    _relayout_stack(node, old)


def _map(old: Rect, new: Rect, p: Point) -> Point:
    sx = new.width / (old.width or 1)
    sy = new.height / (old.height or 1)
    return new.x + (p[0] - old.x) * sx, new.y + (p[1] - old.y) * sy


def _relayout_stack(node: Frame, old: Rect) -> None:
    """Children side by side (or stacked): keep the gutters, share the new span in proportion."""
    new = node.rect
    horizontal = node.split_axis == "horizontal"
    kids = sorted(node.children, key=lambda f: f.rect.y if horizontal else f.rect.x)
    spans = [(k.rect.height if horizontal else k.rect.width) for k in kids]
    starts = [(k.rect.y if horizontal else k.rect.x) for k in kids]
    gaps = [starts[i + 1] - (starts[i] + spans[i]) for i in range(len(kids) - 1)]
    total_old = sum(spans) or 1.0
    total_new = (new.height if horizontal else new.width) - sum(gaps)
    pos = new.y if horizontal else new.x
    for i, kid in enumerate(kids):
        span = spans[i] * total_new / total_old
        rect = Rect(new.x, pos, new.width, span) if horizontal else Rect(pos, new.y, span, new.height)
        relayout(kid, corners(rect))
        pos += span + (gaps[i] if i < len(gaps) else 0)


def move_gutter(node: Frame, index: int, delta: float, gutter: float | None = None, min_span: float = 8.0) -> None:
    """Move the gutter after child `index` (in position order) by `delta` mm; optionally set its width."""
    line = cut_line(node)
    if line is not None and len(node.children) == 2:
        p0, p1, g = line
        dx, dy = p1[0] - p0[0], p1[1] - p0[1]
        length = math.hypot(dx, dy) or 1.0
        horizontal = abs(dx) >= abs(dy)
        # move across the line: down for a horizontal cut, right for a vertical one
        nx, ny = (-dy / length, dx / length)
        if (horizontal and ny < 0) or (not horizontal and nx < 0):
            nx, ny = -nx, -ny
        q0 = (p0[0] + nx * delta, p0[1] + ny * delta)
        q1 = (p1[0] + nx * delta, p1[1] + ny * delta)
        new_gutter = g if gutter is None else float(gutter)
        try:
            first, second = cut(shape(node), q0, q1, new_gutter)
        except ValueError as exc:
            raise ValueError("the gutter would leave a panel too small") from exc
        for pts in (first, second):
            box = bbox(pts)
            if min(box.width, box.height) < min_span:
                raise ValueError("the gutter would leave a panel too small")
        node.split = {"a": _norm(node.rect, q0), "b": _norm(node.rect, q1), "gutter_mm": round(new_gutter, 3)}
        relayout(node)
        return
    horizontal = node.split_axis == "horizontal"
    kids = sorted(node.children, key=lambda f: f.rect.y if horizontal else f.rect.x)
    if not 0 <= index < len(kids) - 1:
        raise ValueError("no gutter there")
    a, b = kids[index], kids[index + 1]
    ra, rb = a.rect, b.rect
    if horizontal:
        old_gap = rb.y - (ra.y + ra.height)
        gap = old_gap if gutter is None else float(gutter)
        top, bottom = ra.y, rb.y + rb.height
        cut_at = ra.y + ra.height + old_gap / 2 + delta
        a_rect = Rect(ra.x, top, ra.width, cut_at - gap / 2 - top)
        b_rect = Rect(rb.x, cut_at + gap / 2, rb.width, bottom - (cut_at + gap / 2))
        small = min(a_rect.height, b_rect.height)
    else:
        old_gap = rb.x - (ra.x + ra.width)
        gap = old_gap if gutter is None else float(gutter)
        left, right = ra.x, rb.x + rb.width
        cut_at = ra.x + ra.width + old_gap / 2 + delta
        a_rect = Rect(left, ra.y, cut_at - gap / 2 - left, ra.height)
        b_rect = Rect(cut_at + gap / 2, rb.y, right - (cut_at + gap / 2), rb.height)
        small = min(a_rect.width, b_rect.width)
    if small < min_span:
        raise ValueError("the gutter would leave a panel too small")
    relayout(a, corners(a_rect))
    relayout(b, corners(b_rect))


def gutters(root: Frame) -> list[dict]:
    """Every gutter a person can drag: {node, index, p0, p1, width, horizontal} (page mm, along its middle)."""
    out: list[dict] = []

    def walk(node: Frame) -> None:
        if not node.children:
            return
        line = cut_line(node)
        if line is not None and len(node.children) == 2:
            p0, p1, g = line
            a, b = _extend_to(shape(node), p0, p1)
            out.append({"node": node.id, "index": 0, "p0": a, "p1": b, "width": g,
                        "horizontal": abs(p1[0] - p0[0]) >= abs(p1[1] - p0[1])})
        else:
            horizontal = node.split_axis == "horizontal"
            kids = sorted(node.children, key=lambda f: f.rect.y if horizontal else f.rect.x)
            for i in range(len(kids) - 1):
                ra, rb = kids[i].rect, kids[i + 1].rect
                if horizontal:
                    g = rb.y - (ra.y + ra.height)
                    y = ra.y + ra.height + g / 2
                    x0, x1 = min(ra.x, rb.x), max(ra.x + ra.width, rb.x + rb.width)
                    out.append({"node": node.id, "index": i, "p0": (x0, y), "p1": (x1, y), "width": g, "horizontal": True})
                else:
                    g = rb.x - (ra.x + ra.width)
                    x = ra.x + ra.width + g / 2
                    y0, y1 = min(ra.y, rb.y), max(ra.y + ra.height, rb.y + rb.height)
                    out.append({"node": node.id, "index": i, "p0": (x, y0), "p1": (x, y1), "width": g, "horizontal": False})
        for child in node.children:
            walk(child)

    walk(root)
    return out


def _extend_to(points: list[Point], p0: Point, p1: Point) -> tuple[Point, Point]:
    """The part of the (infinite) line p0→p1 inside the shape."""
    box = bbox(points)
    a, b = _extend(box, p0, p1)
    return a, b


def distance_to_segment(p: Point, a: Point, b: Point) -> float:
    dx, dy = b[0] - a[0], b[1] - a[1]
    seg = dx * dx + dy * dy
    t = 0.0 if seg == 0 else max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / seg))
    return math.hypot(p[0] - (a[0] + t * dx), p[1] - (a[1] + t * dy))
