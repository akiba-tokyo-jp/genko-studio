"""ノンブル (page numbers): where they go, in what face and size, from which number, and the hidden
nombre (隠しノンブル) printers use to keep pages in order.

Settings live on the book (`episode.nombre`), and a page can hide its own (`page.numero = False`):
{"position": bottom_center | bottom_outside | top_outside | side_outside, "font": a face key
(genko.fonts, gothic by default), "size_mm": 3.0, "start": the number of page 1 (1), "hidden": true to add
the hidden nombre in the gutter, "hidden_size_mm": 2.0, "show": false to print no visible nombre}.
"Outside" is the fore-edge (away from the binding), so it moves side with each page.
"""

from __future__ import annotations

POSITIONS = ("bottom_center", "bottom_outside", "top_outside", "side_outside")
LABELS = {"bottom_center": "下の真ん中", "bottom_outside": "下の外側", "top_outside": "上の外側", "side_outside": "外側の真ん中"}
DEFAULTS = {"position": "bottom_center", "font": "gothic", "size_mm": 3.0, "start": 1, "hidden": False,
            "hidden_size_mm": 2.0, "show": True}


def settings(episode) -> dict:
    return {**DEFAULTS, **(getattr(episode, "nombre", None) or {})} if episode is not None else dict(DEFAULTS)


def validate(change: dict) -> None:
    if "position" in change and change["position"] not in POSITIONS:
        raise ValueError(f"position must be one of {', '.join(POSITIONS)}")
    if "size_mm" in change and not 1 <= float(change["size_mm"]) <= 20:
        raise ValueError("size_mm is 1 to 20")
    if "hidden_size_mm" in change and not 1 <= float(change["hidden_size_mm"]) <= 10:
        raise ValueError("hidden_size_mm is 1 to 10")
    if "start" in change and int(change["start"]) < 0:
        raise ValueError("start is 0 or more")
    if "font" in change and change["font"]:
        from genko import fonts

        if change["font"] not in fonts.BUNDLED:
            raise ValueError(f"font must be one of {', '.join(fonts.BUNDLED)}")


def number(episode, page) -> int:
    return int(settings(episode)["start"]) - 1 + page.index


def placements(episode, page) -> list[dict]:
    """What to print: [{"text", "x_mm", "y_mm" (the middle of the text), "size_mm", "hidden"}]."""
    if not getattr(page, "numero", True):
        return []
    cfg = settings(episode)
    spec = page.spec
    bleed = spec.bleed_mm
    trim = (bleed, bleed, spec.width_mm - bleed, spec.height_mm - bleed)  # x0, y0, x1, y1
    inner = page.inner_rect_mm()
    side = page.side(getattr(episode, "start_side", None))
    outer_right = side == "right"  # a right-hand page's fore-edge is on its right
    text = str(number(episode, page))
    size = float(cfg["size_mm"])
    out = []
    if cfg["show"]:
        below = (inner.y + inner.height + trim[3]) / 2
        above = (trim[1] + inner.y) / 2
        outside_x = (inner.x + inner.width + trim[2]) / 2 if outer_right else (trim[0] + inner.x) / 2
        near_edge_x = inner.x + inner.width - size if outer_right else inner.x + size
        position = cfg["position"]
        if position == "bottom_outside":
            x, y = near_edge_x, below
        elif position == "top_outside":
            x, y = near_edge_x, above
        elif position == "side_outside":
            x, y = outside_x, inner.y + inner.height / 2
        else:
            x, y = spec.width_mm / 2, below
        out.append({"text": text, "x_mm": round(x, 3), "y_mm": round(y, 3), "size_mm": size, "hidden": False})
    if cfg["hidden"]:
        small = float(cfg["hidden_size_mm"])
        gutter_x = trim[0] + small if outer_right else trim[2] - small  # inside the trim, at the binding
        out.append({"text": text, "x_mm": round(gutter_x, 3), "y_mm": round(trim[3] - 15, 3), "size_mm": small, "hidden": True})
    return out


def draw(image, episode, page, dpi: int, colour=(20, 20, 20)) -> None:
    """The page's nombres onto a page image."""
    from PIL import ImageDraw

    from genko import fonts
    from genko.render import mm_to_px

    cfg = settings(episode)
    face = fonts.face(cfg.get("font") or "gothic", "gothic")
    draw_ = ImageDraw.Draw(image)
    for item in placements(episode, page):
        size = max(6, mm_to_px(item["size_mm"], dpi))
        font = face.font(size, "0")
        box = draw_.textbbox((0, 0), item["text"], font=font)
        w, h = box[2] - box[0], box[3] - box[1]
        x = mm_to_px(item["x_mm"], dpi) - w / 2 - box[0]
        y = mm_to_px(item["y_mm"], dpi) - h / 2 - box[1]
        draw_.text((x, y), item["text"], fill=colour, font=font)
