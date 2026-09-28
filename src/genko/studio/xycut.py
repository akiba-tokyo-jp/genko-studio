"""Panel detection on a hand-drawn name (atari) by recursive XY-cut (Pillow only).

1. Binarize (Otsu) and thicken strokes a little so broken pen lines stay closed.
2. Project the ink onto both axes; a gutter is a run of (almost) empty rows or
   columns at least `min_gutter_mm` wide. Cut at the widest gutter, preferring
   horizontal cuts (tiers) at the top level, and recurse into both sides.
3. Each leaf is shrunk to its ink box; a panel's border shows up as dark rows
   and columns at the box edges. The share of the box's perimeter that is ink
   is the panel's `border` score; the gutters' emptiness is its `gutter` score.
   The confidence is their product.

The result is a proposal only: rectangles in page millimetres and the cut tree
(which maps onto tiers → columns → rows when it is at most three levels deep).
"""

from __future__ import annotations

from dataclasses import dataclass, field

from PIL import Image, ImageFilter

Box = tuple[int, int, int, int]  # x0, y0, x1, y1 in pixels (x1, y1 exclusive)


@dataclass
class Params:
    min_gutter_mm: float = 0.6      # narrower gaps are not gutters (vertical gutters are often only 2–3 mm)
    min_panel_mm: float = 12.0      # smaller pieces are not panels (text, specks)
    gutter_px: int = 1              # a gutter row/column may hold this many ink pixels (after speck removal)
    speck_mm: float = 3.0           # ink blobs smaller than this (both ways) are dropped: dirt, eraser crumbs, dots
    thicken_mm: float = 0.3
    work_px_per_mm: float = 4.0     # detection resolution (about 100 dpi)


@dataclass
class Node:
    box: Box
    axis: str | None = None         # "horizontal" (stacked top to bottom) or "vertical" (side by side)
    children: list["Node"] = field(default_factory=list)
    gutter: float = 1.0              # how clean the cuts around this node were (0..1)
    border: float = 0.0              # share of the leaf's perimeter that is ink (0..1)

    def leaves(self) -> list["Node"]:
        if not self.children:
            return [self]
        return [leaf for child in self.children for leaf in child.leaves()]


def otsu(gray: Image.Image) -> int:
    hist = gray.histogram()
    total = sum(hist)
    sum_all = sum(i * h for i, h in enumerate(hist))
    best, best_t, weight_b, sum_b = 0.0, 128, 0, 0.0
    for t in range(256):
        weight_b += hist[t]
        if weight_b == 0:
            continue
        weight_f = total - weight_b
        if weight_f == 0:
            break
        sum_b += t * hist[t]
        mean_b = sum_b / weight_b
        mean_f = (sum_all - sum_b) / weight_f
        between = weight_b * weight_f * (mean_b - mean_f) ** 2
        if between > best:
            best, best_t = between, t
    return best_t


def binarize(image: Image.Image, dpi: float, params: Params) -> Image.Image:
    """L image, 255 = ink."""
    gray = image.convert("L")
    t = min(otsu(gray), 200)
    ink = gray.point(lambda v: 255 if v <= t else 0)
    grow = max(1, round(params.thicken_mm / 25.4 * dpi))
    if grow > 1:
        ink = ink.filter(ImageFilter.MaxFilter(grow if grow % 2 else grow + 1))
    return ink


def drop_specks(ink: Image.Image, max_px: int) -> Image.Image:
    """Remove 8-connected ink blobs whose bounding box is smaller than max_px in both directions."""
    w, h = ink.size
    data = bytearray(ink.tobytes())
    seen = bytearray(w * h)
    for start in range(w * h):
        if not data[start] or seen[start]:
            continue
        stack = [start]
        seen[start] = 1
        blob = []
        x0 = x1 = start % w
        y0 = y1 = start // w
        while stack:
            i = stack.pop()
            blob.append(i)
            y, x = divmod(i, w)
            x0, x1, y0, y1 = min(x0, x), max(x1, x), min(y0, y), max(y1, y)
            for j in (i - w - 1, i - w, i - w + 1, i - 1, i + 1, i + w - 1, i + w, i + w + 1):
                if 0 <= j < w * h and data[j] and not seen[j] and abs(j % w - x) <= 1:
                    seen[j] = 1
                    stack.append(j)
        if x1 - x0 < max_px and y1 - y0 < max_px:
            for i in blob:
                data[i] = 0
    return Image.frombytes("L", (w, h), bytes(data))


def _profile(ink: Image.Image, box: Box, axis: str) -> list[int]:
    """Ink pixel counts per row (axis horizontal) or per column (axis vertical) inside box."""
    region = ink.crop(box)
    w, h = region.size
    if axis == "horizontal":
        rows = region.resize((1, h), Image.Resampling.BOX)
        return [round(v * w / 255) for v in rows.tobytes()]
    cols = region.resize((w, 1), Image.Resampling.BOX)
    return [round(v * h / 255) for v in cols.tobytes()]


def _gaps(profile: list[int], limit: float, min_run: int) -> list[tuple[int, int]]:
    """Runs of near-empty entries (not touching either end) at least min_run long."""
    gaps, start = [], None
    for i, value in enumerate(profile + [limit + 1]):
        empty = value <= limit
        if empty and start is None:
            start = i
        elif not empty and start is not None:
            if start > 0 and i < len(profile) and i - start >= min_run:
                gaps.append((start, i))
            start = None
    return gaps


def _flanked(ink: Image.Image, box: Box, axis: str, a: int, b: int, px_per_mm: float, cover: float = 0.5) -> bool:
    """A gutter between panels runs between two panel borders: on each side, within a few mm,
    some line parallel to the gutter covers at least `cover` of its length."""
    x0, y0, x1, y1 = box
    band = max(2, round(3.0 * px_per_mm))
    if axis == "horizontal":  # a horizontal gutter at rows a..b: look for long horizontal lines above and below
        sides = [(x0, y0 + max(0, a - band), x1, y0 + a), (x0, y0 + b, x1, y0 + min(y1 - y0, b + band))]
        across = "vertical"
    else:
        sides = [(x0 + max(0, a - band), y0, x0 + a, y1), (x0 + b, y0, x0 + min(x1 - x0, b + band), y1)]
        across = "horizontal"
    for side in sides:
        if side[2] <= side[0] or side[3] <= side[1]:
            return False
        region = ink.crop(side)
        # the best single line in the band: per row (or column) of the band, the share of its length inked
        lines = region.transpose(Image.Transpose.TRANSPOSE) if across == "vertical" else region
        w, h = lines.size
        best = 0.0
        for i in range(w):
            column = lines.crop((i, 0, i + 1, h))
            best = max(best, sum(1 for v in column.tobytes() if v) / max(1, h))
        if best < cover:
            return False
    return True


def _trim(ink: Image.Image, box: Box) -> Box | None:
    bbox = ink.crop(box).getbbox()
    if bbox is None:
        return None
    x0, y0, _, _ = box
    return (x0 + bbox[0], y0 + bbox[1], x0 + bbox[2], y0 + bbox[3])


def _border(ink: Image.Image, box: Box, band: int) -> float:
    """Share of the four edges (a `band` px strip each) that is inked along their length."""
    x0, y0, x1, y1 = box
    scores = []
    for strip, axis in (((x0, y0, x1, min(y1, y0 + band)), "vertical"), ((x0, max(y0, y1 - band), x1, y1), "vertical"),
                        ((x0, y0, min(x1, x0 + band), y1), "horizontal"), ((max(x0, x1 - band), y0, x1, y1), "horizontal")):
        if strip[2] <= strip[0] or strip[3] <= strip[1]:
            continue
        profile = _profile(ink, strip, axis)
        scores.append(sum(1 for v in profile if v > 0) / max(1, len(profile)))
    return round(sum(scores) / len(scores), 3) if scores else 0.0


def cut(ink: Image.Image, box: Box, px_per_mm: float, params: Params, depth: int = 0, prefer: str = "horizontal") -> Node | None:
    trimmed = _trim(ink, box)
    if trimmed is None:
        return None
    x0, y0, x1, y1 = trimmed
    min_panel = params.min_panel_mm * px_per_mm
    if (x1 - x0) < min_panel or (y1 - y0) < min_panel:
        return None
    min_gap = max(2, round(params.min_gutter_mm * px_per_mm))
    candidates = []
    ends = round(4.0 * px_per_mm)  # corners overshoot into gutters: ignore the region's two ends when looking for one
    for axis in ("horizontal", "vertical"):
        if axis == "horizontal":
            inner = (x0 + ends, y0, x1 - ends, y1) if x1 - x0 > 3 * ends else trimmed
        else:
            inner = (x0, y0 + ends, x1, y1 - ends) if y1 - y0 > 3 * ends else trimmed
        profile = _profile(ink, inner, axis)
        for a, b in _gaps(profile, params.gutter_px, min_gap):
            if not _flanked(ink, trimmed, axis, a, b, px_per_mm):
                continue  # gaps in the pen line that happen to line up, not a gutter between panels
            stray = sum(profile[a:b]) / max(1, b - a)
            candidates.append((axis, a, b, round(max(0.0, 1.0 - stray / (params.gutter_px + 1)), 3)))
    if candidates and depth < 8:
        # cut at every gutter of the preferred axis if it has any (tiers first, then columns)
        axis = prefer if any(c[0] == prefer for c in candidates) else ("vertical" if prefer == "horizontal" else "horizontal")
        cuts = sorted((c for c in candidates if c[0] == axis), key=lambda c: c[1])
        pieces: list[Box] = []
        start = 0
        span = (y1 - y0) if axis == "horizontal" else (x1 - x0)
        for _, a, b, _ in cuts:
            pieces.append((start, a))
            start = b
        pieces.append((start, span))
        children = []
        for a, b in pieces:
            sub = (x0, y0 + a, x1, y0 + b) if axis == "horizontal" else (x0 + a, y0, x0 + b, y1)
            node = cut(ink, sub, px_per_mm, params, depth + 1, "vertical" if axis == "horizontal" else "horizontal")
            if node is not None:
                children.append(node)
        if len(children) >= 2:
            gutter = min(c[3] for c in cuts)
            return Node(trimmed, axis, children, gutter=round(gutter, 3))
        if len(children) == 1:
            return children[0]
    band = max(2, round(1.2 * px_per_mm))
    return Node(trimmed, border=_border(ink, trimmed, band))


@dataclass
class Detection:
    leaves_mm: list[dict]           # [{rect_mm, confidence, border, gutter}] in reading order
    tree: dict                      # {rect_mm, axis?, children?} in page mm
    tiers: list | None              # tiers DSL when the tree fits (≤ 3 levels), else None
    threshold: int
    confidence: float


def _to_mm(box: Box, placement: tuple[float, float, float, float], size: tuple[int, int]) -> list[float]:
    px, py, pw, ph = placement
    sx, sy = pw / size[0], ph / size[1]
    x0, y0, x1, y1 = box
    return [round(px + x0 * sx, 2), round(py + y0 * sy, 2), round((x1 - x0) * sx, 2), round((y1 - y0) * sy, 2)]


def _reading_order(node: Node, right_to_left: bool) -> list[Node]:
    if not node.children:
        return [node]
    kids = list(node.children)
    if node.axis == "vertical" and right_to_left:
        kids.reverse()
    return [leaf for kid in kids for leaf in _reading_order(kid, right_to_left)]


def _tree_dict(node: Node, placement, size, parent_gutter: float = 1.0) -> dict:
    out = {"rect_mm": _to_mm(node.box, placement, size)}
    if node.children:
        out["axis"] = node.axis
        out["children"] = [_tree_dict(c, placement, size, min(parent_gutter, node.gutter)) for c in node.children]
    return out


def _tiers(root: Node, right_to_left: bool) -> list | None:
    """tiers (top to bottom) → cols (listed right to left) → rows, as in the name plan DSL."""
    tiers_nodes = root.children if root.axis == "horizontal" else [root]
    total_h = sum(n.box[3] - n.box[1] for n in tiers_nodes)
    tiers = []
    slot = 0
    for tier in tiers_nodes:
        cols_nodes = tier.children if tier.axis == "vertical" else [tier]
        if right_to_left:
            cols_nodes = list(reversed(cols_nodes))
        total_w = sum(n.box[2] - n.box[0] for n in cols_nodes)
        cols = []
        for col in cols_nodes:
            rows = None
            if col.children:
                if col.axis != "horizontal" or any(r.children for r in col.children):
                    return None
                total_r = sum(r.box[3] - r.box[1] for r in col.children)
                rows = []
                for r in col.children:
                    slot += 1
                    rows.append({"slot": f"p{slot}", "h": round((r.box[3] - r.box[1]) / total_r, 3)})
            else:
                slot += 1
            cols.append({"slot": None if rows else f"p{slot}", "w": round((col.box[2] - col.box[0]) / total_w, 3), "rows": rows})
        tiers.append({"h": round((tier.box[3] - tier.box[1]) / total_h, 3), "cols": cols})
    for tier in tiers:
        for col in tier["cols"]:
            if col["slot"] is None:
                col["slot"] = col["rows"][0]["slot"]
    return tiers


def detect(image: Image.Image, placement_mm: tuple[float, float, float, float], params: Params | None = None,
           right_to_left: bool = True) -> Detection:
    """Panels of the scan `image`, which covers `placement_mm` (x, y, w, h) of the page."""
    params = params or Params()
    threshold = otsu(image.convert("L"))
    # work at a fixed resolution: fast, and the thresholds mean the same at any scan dpi
    work = (max(1, round(placement_mm[2] * params.work_px_per_mm)), max(1, round(placement_mm[3] * params.work_px_per_mm)))
    small = image.convert("L").resize(work, Image.Resampling.BOX) if image.size != work else image.convert("L")
    size = small.size
    px_per_mm = size[0] / placement_mm[2]
    ink = binarize(small, px_per_mm * 25.4, params)
    ink = drop_specks(ink, max(2, round(params.speck_mm * px_per_mm)))
    root = cut(ink, (0, 0, size[0], size[1]), px_per_mm, params)
    if root is None:
        return Detection([], {}, None, threshold, 0.0)
    leaves = []
    for leaf in _reading_order(root, right_to_left):
        confidence = round(min(1.0, leaf.border * 1.1) * (0.5 + 0.5 * root.gutter if root.children else 1.0), 3)
        leaves.append({"rect_mm": _to_mm(leaf.box, placement_mm, size), "confidence": confidence, "border": leaf.border})
    overall = round(sum(leaf["confidence"] for leaf in leaves) / len(leaves), 3)
    return Detection(leaves, _tree_dict(root, placement_mm, size), _tiers(root, right_to_left), threshold, overall)


def content_box(image: Image.Image, dpi_guess: float = 150.0) -> Box | None:
    """The box of the drawing on a scan (ignoring specks), for aligning it to the page's live area."""
    ink = binarize(image, dpi_guess, Params(thicken_mm=0.3))
    w, h = ink.size
    rows = _profile(ink, (0, 0, w, h), "horizontal")
    cols = _profile(ink, (0, 0, w, h), "vertical")
    row_min = max(2, round(w * 0.02))
    col_min = max(2, round(h * 0.02))
    ys = [i for i, v in enumerate(rows) if v >= row_min]
    xs = [i for i, v in enumerate(cols) if v >= col_min]
    if not xs or not ys:
        return None
    return (xs[0], ys[0], xs[-1] + 1, ys[-1] + 1)
