"""Covers (表紙・裏表紙・カバー): pages at the end of the book marked `page.extra["cover"]` = {"kind": front |
back | jacket, "spine_mm", "flap_mm"}. Front and back covers are book-sized; a jacket (カバー) is one wide sheet
with both covers, the spine between them and the flaps (袖) at its ends, laid out as it prints: for a book bound
on the right (右綴じ) [flap][front][spine][back][flap], on the left the other way round.

Covers have no nombre and do not count as pages; the book preview and the exports put them first and last.
"""

from __future__ import annotations

import dataclasses

KINDS = ("front", "back", "jacket")
LABELS = {"front": "表紙", "back": "裏表紙", "jacket": "カバー（表紙・背・裏表紙・袖）"}


def cover_of(page) -> dict | None:
    data = (getattr(page, "extra", None) or {}).get("cover")
    return data if isinstance(data, dict) and data.get("kind") in KINDS else None


def is_cover(page) -> bool:
    return cover_of(page) is not None


def spec_for(book, cover: dict):
    """The paper of a cover: a book page for the front and back; for a jacket, the width of both covers, the
    spine and the flaps (the same bleed and paper allowance as the book)."""
    if cover.get("kind") != "jacket":
        return book
    trim_w, trim_h = book.trim_size()
    spine, flap = float(cover.get("spine_mm") or 0), float(cover.get("flap_mm") or 0)
    width = 2 * trim_w + spine + 2 * flap
    allowance_w = book.width_mm - trim_w
    return dataclasses.replace(book, width_mm=round(width + allowance_w, 3), trim_w_mm=round(width, 3), trim_h_mm=trim_h,
                               preset="cover", margins_mm=None)


def folds(page, binding: str = "right") -> list[tuple[float, float, str]]:
    """The parts of a jacket across the page (x0, x1 in mm, name), from the left, and so where it folds."""
    cover = cover_of(page)
    if not cover or cover.get("kind") != "jacket":
        return []
    t = page.trim_rect_mm()
    spine, flap = float(cover.get("spine_mm") or 0), float(cover.get("flap_mm") or 0)
    face = (t.width - spine - 2 * flap) / 2
    order = ["袖", "表紙", "背", "裏表紙", "袖"] if binding == "right" else ["袖", "裏表紙", "背", "表紙", "袖"]
    widths = [flap, face, spine, face, flap]
    out, x = [], t.x
    for name, w in zip(order, widths):
        if w > 0:
            out.append((round(x, 3), round(x + w, 3), name))
        x += w
    return out


def pages_in_order(episode) -> list:
    """The book as it is read: front cover (or jacket), the pages, the back cover."""
    covers = {cover_of(p)["kind"]: p for p in episode.pages if is_cover(p)}
    body = [p for p in episode.pages if not is_cover(p)]
    out = []
    if "jacket" in covers:
        out.append(covers["jacket"])
    elif "front" in covers:
        out.append(covers["front"])
    out += body
    if "back" in covers and "jacket" not in covers:
        out.append(covers["back"])
    return out


def file_stem(page) -> str:
    """The part of an exported file's name for this page: p003, or cover_front / cover_back / cover_jacket."""
    cover = cover_of(page)
    return f"cover_{cover['kind']}" if cover else f"p{page.index:03d}"


def front_of(page, image, dpi: int, binding: str = "right"):
    """The front cover cut out of a jacket's picture (the page itself for a front cover)."""
    from genko.render import mm_to_px

    part = next(((x0, x1) for x0, x1, name in folds(page, binding) if name == "表紙"), None)
    if part is None:
        return image
    t = page.trim_rect_mm()
    return image.crop((mm_to_px(part[0], dpi), mm_to_px(t.y, dpi), mm_to_px(part[1], dpi), mm_to_px(t.y + t.height, dpi)))


def draw_folds(image, page, dpi: int, binding: str = "right") -> None:
    """Where a jacket folds (dashed) and what each part is, for the name and proof views."""
    from PIL import ImageDraw

    from genko.render import mm_to_px

    parts = folds(page, binding)
    if not parts:
        return
    draw = ImageDraw.Draw(image)
    t = page.trim_rect_mm()
    y0, y1 = mm_to_px(t.y, dpi), mm_to_px(t.y + t.height, dpi)
    for x0, x1, name in parts:
        for x in (x0, x1):
            px = mm_to_px(x, dpi)
            for y in range(y0, y1, max(4, mm_to_px(4, dpi))):
                draw.line([(px, y), (px, min(y1, y + max(2, mm_to_px(2, dpi))))], fill=(60, 140, 220), width=1)
        from genko import fonts

        size = max(10, mm_to_px(4, dpi))
        draw.text(((mm_to_px(x0, dpi) + mm_to_px(x1, dpi)) / 2, y0 + size), name, fill=(60, 140, 220),
                  font=fonts.face(None).font(size, name[0]), anchor="mm")
