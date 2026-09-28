"""Panel guides for image tools: composition, pose, keepout, mask and compare.

Every guide is drawn at the generation size (px) over the generation box: the
panel rect grown by `pad_mm` on each side. White ground, simple black or grey
shapes, so any image tool can take them as reference images. No balloons,
borders of other panels or speaker names.
"""

from __future__ import annotations

import io
from dataclasses import dataclass

from PIL import Image, ImageDraw, ImageFont

from genko.models import Frame, LayerRole, Page, Rect

Box = tuple[float, float, float, float]


@dataclass(frozen=True)
class GenBox:
    """The page area (mm) an image of `px` covers."""

    x: float
    y: float
    width: float
    height: float
    px: tuple[int, int]

    @classmethod
    def for_frame(cls, frame: Frame, pad_mm: float, px: tuple[int, int]) -> "GenBox":
        r = frame.rect
        return cls(r.x - pad_mm, r.y - pad_mm, r.width + 2 * pad_mm, r.height + 2 * pad_mm, (int(px[0]), int(px[1])))

    def to_px(self, x_mm: float, y_mm: float) -> tuple[float, float]:
        return ((x_mm - self.x) * self.px[0] / self.width, (y_mm - self.y) * self.px[1] / self.height)

    def box_px(self, box: Box) -> tuple[float, float, float, float]:
        x0, y0 = self.to_px(box[0], box[1])
        x1, y1 = self.to_px(box[0] + box[2], box[1] + box[3])
        return (x0, y0, x1, y1)

    def box01(self, box: Box) -> list[float]:
        x0 = (box[0] - self.x) / self.width
        y0 = (box[1] - self.y) / self.height
        w = box[2] / self.width
        h = box[3] / self.height
        return [round(max(0.0, x0), 3), round(max(0.0, y0), 3), round(min(1.0, w), 3), round(min(1.0, h), 3)]

    def rect(self) -> Rect:
        return Rect(self.x, self.y, self.width, self.height)


def _line_width(box: GenBox, mm: float) -> int:
    return max(1, round(mm * box.px[0] / box.width))


def figures_for(frame: Frame) -> list:
    from genko.studio.blocking import figures

    r = frame.rect
    return figures(frame.panel or {}, (r.x, r.y, r.width, r.height))


def _name_strokes(page: Page) -> list:
    for layer in page.layers:
        if layer.role == LayerRole.NAME:
            return list(layer.strokes)
    return []


def composition(page: Page, frame: Frame, box: GenBox) -> Image.Image:
    """The name for this panel: its border, the name strokes, and rough figures."""
    image = Image.new("L", box.px, 255)
    draw = ImageDraw.Draw(image)
    lw = _line_width(box, 0.5)
    draw.rectangle(box.box_px((frame.rect.x, frame.rect.y, frame.rect.width, frame.rect.height)), outline=0, width=lw)
    for stroke in _name_strokes(page):
        points = [box.to_px(float(p[0]), float(p[1])) for p in stroke.points]
        if len(points) >= 2:
            draw.line(points, fill=0, width=_line_width(box, max(0.3, stroke.width_mm)))
    for fig in figures_for(frame):
        hx0, hy0, hx1, hy1 = box.box_px(fig.head)
        draw.ellipse((hx0, hy0, hx1, hy1), outline=0, width=lw)
        bx0, _, bx1, by1 = box.box_px(fig.body)
        neck = hy1
        draw.line([((hx0 + hx1) / 2, neck), ((hx0 + hx1) / 2, by1)], fill=0, width=lw)
        draw.line([(bx0, neck + (by1 - neck) * 0.15), (bx1, neck + (by1 - neck) * 0.15)], fill=0, width=lw)
    return image


def pose(page: Page, frame: Frame, box: GenBox) -> Image.Image:
    """Where each character stands: body box, head, facing arrow and id."""
    image = Image.new("L", box.px, 255)
    draw = ImageDraw.Draw(image)
    lw = _line_width(box, 0.6)
    chars = {c.get("id"): c for c in (frame.panel or {}).get("characters", []) if isinstance(c, dict)}
    font = ImageFont.load_default()
    for fig in figures_for(frame):
        draw.rectangle(box.box_px(fig.body), outline=128, width=lw)
        hx0, hy0, hx1, hy1 = box.box_px(fig.head)
        draw.ellipse((hx0, hy0, hx1, hy1), fill=200, outline=0, width=lw)
        cx, cy = (hx0 + hx1) / 2, (hy0 + hy1) / 2
        facing = chars.get(fig.char_id, {}).get("facing", "front")
        length = (hx1 - hx0) * 0.9
        if facing in ("left", "right"):
            sign = -1 if facing == "left" else 1
            tip = (cx + sign * length, cy)
            draw.line([(cx, cy), tip], fill=0, width=lw)
            draw.polygon([tip, (tip[0] - sign * lw * 4, cy - lw * 3), (tip[0] - sign * lw * 4, cy + lw * 3)], fill=0)
        elif facing == "front":
            draw.ellipse((cx - lw * 2, cy - lw * 2, cx + lw * 2, cy + lw * 2), fill=0)
        else:
            draw.line([(cx - lw * 3, cy - lw * 3), (cx + lw * 3, cy + lw * 3)], fill=0, width=lw)
            draw.line([(cx - lw * 3, cy + lw * 3), (cx + lw * 3, cy - lw * 3)], fill=0, width=lw)
        draw.text((hx0, max(0, hy0 - 12)), str(fig.char_id), fill=0, font=font)
    # posed mannequins placed in this panel are the pose itself: draw them over the boxes
    from genko import mannequin

    r = frame.rect
    for prim in page.prims:
        if prim.get("kind") != "mannequin" or not mannequin.in_rect(prim, (r.x, r.y, r.width, r.height)):
            continue
        bone = mannequin.skeleton(prim)
        for a, b, part in bone["segments"]:
            draw.line([box.to_px(*a), box.to_px(*b)], fill=0 if part == "body" else 60, width=lw * 2)
        (hx, hy), hr = bone["head"]
        draw.ellipse(box.box_px((hx - hr, hy - hr, hr * 2, hr * 2)), outline=0, width=lw * 2)
    return image


def has_mannequin(page: Page, frame: Frame) -> bool:
    from genko import mannequin

    r = frame.rect
    return any(p.get("kind") == "mannequin" and mannequin.in_rect(p, (r.x, r.y, r.width, r.height)) for p in page.prims)


def keepout_boxes(episode, page: Page, frame: Frame, grow_mm: float = 2.0) -> list[tuple[Box, str]]:
    """Balloon areas in this panel, grown a little: the image should stay calm there."""
    out = []
    for i, line in enumerate(ln for ln in episode.story_for_page(page.index) if ln.frame_id == frame.id):
        box = (line.x_mm - grow_mm, line.y_mm - grow_mm, line.w_mm + 2 * grow_mm, line.h_mm + 2 * grow_mm)
        out.append((box, f"台詞（読み順 {i + 1}）"))
    return out


def keepout(episode, page: Page, frame: Frame, box: GenBox) -> Image.Image:
    image = Image.new("L", box.px, 255)
    draw = ImageDraw.Draw(image)
    for rect, _ in keepout_boxes(episode, page, frame):
        draw.rectangle(box.box_px(rect), fill=160)
    return image


def mask(box: GenBox, regions_mm: list[Box]) -> Image.Image:
    """Inpaint mask: white where the image tool may redraw, black elsewhere."""
    image = Image.new("L", box.px, 0)
    draw = ImageDraw.Draw(image)
    for rect in regions_mm:
        draw.rectangle(box.box_px(rect), fill=255)
    return image


def fit_cover(image: Image.Image, size: tuple[int, int]) -> Image.Image:
    ratio = max(size[0] / image.width, size[1] / image.height)
    scaled = image.resize((max(1, round(image.width * ratio)), max(1, round(image.height * ratio))), Image.Resampling.LANCZOS)
    left = (scaled.width - size[0]) // 2
    top = (scaled.height - size[1]) // 2
    return scaled.crop((left, top, left + size[0], top + size[1]))


def compare(candidate: Image.Image, page: Page, frame: Frame, box: GenBox) -> Image.Image:
    """The candidate (cover-fitted to the generation box) with the composition in red at 50%."""
    base = fit_cover(candidate.convert("RGB"), box.px)
    lines = composition(page, frame, box)
    red = Image.new("RGB", box.px, (230, 20, 20))
    alpha = lines.point(lambda v: 128 if v < 128 else 0)
    base.paste(red, (0, 0), alpha)
    return base


def to_png(image: Image.Image) -> bytes:
    buf = io.BytesIO()
    image.save(buf, format="PNG", optimize=False)
    return buf.getvalue()


def ink_ratio(image: Image.Image) -> float:
    gray = image.convert("L")
    hist = gray.histogram()
    return round(sum(hist[:128]) / max(1, gray.width * gray.height), 4)
