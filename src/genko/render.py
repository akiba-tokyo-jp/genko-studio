from __future__ import annotations

import io
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFont

from genko.models import Episode, LayerKind, LayerRole, Page, Rect, StoryLine
from genko.tategaki import compose as compose_tategaki

EXPORT_ROLES = (
    LayerRole.INK,
    LayerRole.BG,
    LayerRole.FINISH,
    LayerRole.TONE,
    LayerRole.EFFECT,
    LayerRole.FRAMES,
    LayerRole.TEXT,
)

NAME_COLOR = (58, 110, 165)
INK_COLOR = (20, 20, 20)


def export_plan(page: Page) -> list[LayerRole]:
    return list(EXPORT_ROLES)


def mm_to_px(mm: float, dpi: int) -> int:
    return max(1, round(mm / 25.4 * dpi))


def rect_px(rect: Rect, dpi: int) -> tuple[int, int, int, int]:
    x = mm_to_px(rect.x, dpi)
    y = mm_to_px(rect.y, dpi)
    w = mm_to_px(rect.width, dpi)
    h = mm_to_px(rect.height, dpi)
    return x, y, x + w, y + h


def _xy(point: tuple, dpi: int) -> tuple[int, int]:
    return mm_to_px(float(point[0]), dpi), mm_to_px(float(point[1]), dpi)


def _stroke(
    draw: ImageDraw.ImageDraw,
    points: list[tuple],
    dpi: int,
    color: tuple[int, int, int],
    width: int,
) -> None:
    if len(points) < 2:
        return
    if len(points[0]) >= 3:
        for a, b in zip(points, points[1:]):
            pressure = float(a[2]) if len(a) > 2 else 1.0
            w = max(1, round(width * max(0.15, min(1.5, pressure))))
            draw.line([_xy(a, dpi), _xy(b, dpi)], fill=color, width=w, joint="curve")
        return
    xy = [_xy(pt, dpi) for pt in points]
    draw.line(xy, fill=color, width=width, joint="curve")


def _clip_mask(page: Page, size: tuple[int, int], dpi: int) -> Image.Image | None:
    leaves = [frame for frame in page.leaf_frames() if getattr(frame, "clip", True)]
    if not leaves:
        return None
    mask = Image.new("L", size, 0)
    draw = ImageDraw.Draw(mask)
    for frame in leaves:
        draw.rectangle(rect_px(frame.rect, dpi), fill=255)
    return mask


def _and_alpha(layer: Image.Image, mask: Image.Image) -> Image.Image:
    alpha = layer.split()[3]
    return ImageChops.multiply(alpha, mask)


_DELA = Path(__file__).resolve().parent / "fonts" / "DelaGothicOne-Regular.ttf"  # package data (ships in the wheel)
_CJK_FONTS = (
    str(_DELA),
    r"C:\Windows\Fonts\YuGothM.ttc",
    r"C:\Windows\Fonts\meiryo.ttc",
    r"C:\Windows\Fonts\NotoSansJP-VF.ttf",
    r"C:\Windows\Fonts\msgothic.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
)


def _font(path: str | None = None, size: int = 14) -> ImageFont.ImageFont:
    size = max(8, int(size))
    candidates = [path] if path else []
    candidates.extend(_CJK_FONTS)
    for candidate in candidates:
        if not candidate:
            continue
        try:
            return ImageFont.truetype(candidate, size)
        except (OSError, ValueError):
            continue
    return ImageFont.load_default()


def _open_raster(layer) -> Image.Image | None:
    if layer.raster_png:
        return Image.open(io.BytesIO(layer.raster_png)).convert("RGBA")
    return None


def _blend_over(base: Image.Image, over: Image.Image, mode: str, opacity: float, clip_mask: Image.Image | None = None) -> Image.Image:
    over = over.convert("RGBA")
    if clip_mask is not None:
        r, g, b, a = over.split()
        a = ImageChops.multiply(a, clip_mask.convert("L"))
        over = Image.merge("RGBA", (r, g, b, a))
    if opacity < 1:
        r, g, b, a = over.split()
        a = a.point(lambda p, o=opacity: int(p * o))
        over = Image.merge("RGBA", (r, g, b, a))
    base_rgba = base.convert("RGBA")
    if mode in ("", "normal", None):
        return Image.alpha_composite(base_rgba, over)
    br, bg, bb, ba = base_rgba.split()
    rr, rg, rb, ra = over.split()
    base_rgb = Image.merge("RGB", (br, bg, bb))
    over_rgb = Image.merge("RGB", (rr, rg, rb))
    if mode == "multiply":
        mixed = ImageChops.multiply(base_rgb, over_rgb)
    elif mode == "screen":
        mixed = ImageChops.screen(base_rgb, over_rgb)
    elif mode == "add":
        mixed = ImageChops.add(base_rgb, over_rgb)
    elif mode == "overlay":
        mixed = ImageChops.overlay(base_rgb, over_rgb) if hasattr(ImageChops, "overlay") else ImageChops.multiply(base_rgb, over_rgb)
    else:
        mixed = over_rgb
    mixed_rgba = mixed.convert("RGBA")
    mixed_rgba.putalpha(ra)
    return Image.alpha_composite(base_rgba, mixed_rgba)


def _draw_tone(image: Image.Image, page: Page, dpi: int) -> None:
    draw = ImageDraw.Draw(image)
    for layer in page.layers:
        if layer.role != LayerRole.TONE or not layer.visible:
            continue
        density = float(layer.density or 0.3)
        lpi = float(layer.lpi or 60)
        spacing = max(2, round(dpi / lpi))
        radius = max(1, round(spacing * density * 0.45))
        import math

        angle = math.radians(float(getattr(layer, "angle", 45) or 0))
        ca, sa = math.cos(angle), math.sin(angle)
        frames = page.leaf_frames()
        boxes = [rect_px(frame.rect, dpi) for frame in frames]
        if layer.region:
            xs = [mm_to_px(pt[0], dpi) for pt in layer.region]
            ys = [mm_to_px(pt[1], dpi) for pt in layer.region]
            boxes = [(min(xs), min(ys), max(xs), max(ys))]
        for x0, y0, x1, y1 in boxes:
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            span = int(((x1 - x0) ** 2 + (y1 - y0) ** 2) ** 0.5)
            for i in range(-span, span, spacing):
                for j in range(-span, span, spacing):
                    x = int(cx + i * ca - j * sa)
                    y = int(cy + i * sa + j * ca)
                    if x0 <= x <= x1 and y0 <= y <= y1:
                        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=(20, 20, 20))


def _draw_effects(image: Image.Image, page: Page, dpi: int) -> None:
    draw = ImageDraw.Draw(image)
    for effect in page.effects:
        kind = effect.get("kind")
        frame = None
        if effect.get("frame_id"):
            try:
                frame = page._find(effect["frame_id"])
            except KeyError:
                frame = None
        box = rect_px(frame.rect, dpi) if frame is not None else (0, 0, image.width, image.height)
        x0, y0, x1, y1 = box
        cx = (x0 + x1) // 2
        cy = (y0 + y1) // 2
        count = int(effect.get("params", {}).get("count", 36))
        if kind == "focus":
            import math

            radius = max(x1 - x0, y1 - y0) // 2
            for i in range(count):
                angle = (2 * math.pi * i) / count
                draw.line(
                    (cx, cy, int(cx + radius * math.cos(angle)), int(cy + radius * math.sin(angle))),
                    fill=(20, 20, 20),
                    width=1,
                )
        elif kind == "speed":
            for i in range(count):
                y = y0 + int((y1 - y0) * i / max(1, count - 1))
                draw.line((x0, y, x1, y), fill=(20, 20, 20), width=1)
        elif kind == "white":
            draw.rectangle(box, fill=(255, 255, 255))


def _project_box(prim: dict, dpi: int) -> list[tuple[int, int]]:
    pos = prim.get("pos") or [100, 150, 0]
    size = prim.get("size") or [40, 40, 40]
    rot = prim.get("rot") or [0, 0.6, 0.4]
    cx, cy, cz = (float(v) for v in pos)
    sx, sy, sz = (float(v) / 2 for v in size)
    corners = []
    for dx in (-sx, sx):
        for dy in (-sy, sy):
            for dz in (-sz, sz):
                x, y, z = dx, dy, dz
                ry = rot[1]
                import math

                x2 = x * math.cos(ry) - z * math.sin(ry)
                z2 = x * math.sin(ry) + z * math.cos(ry)
                x, z = x2, z2
                rx = rot[0]
                y2 = y * math.cos(rx) - z * math.sin(rx)
                z2 = y * math.sin(rx) + z * math.cos(rx)
                y, z = y2, z2
                depth = 200 + z
                scale = 180 / max(40, depth)
                corners.append((mm_to_px(cx + x * scale, dpi), mm_to_px(cy + y * scale, dpi)))
    return corners


def _draw_prims(image: Image.Image, page: Page, dpi: int, mode: str) -> None:
    if mode == "print" or not page.prims:
        return
    draw = ImageDraw.Draw(image)
    edges = [
        (0, 1), (1, 3), (3, 2), (2, 0),
        (4, 5), (5, 7), (7, 6), (6, 4),
        (0, 4), (1, 5), (2, 6), (3, 7),
    ]
    for prim in page.prims:
        if prim.get("kind") == "mannequin":
            _draw_mannequin(draw, prim, dpi)
            continue
        pts = _project_box(prim, dpi)
        if len(pts) < 8:
            continue
        for a, b in edges:
            draw.line([pts[a], pts[b]], fill=(90, 90, 140), width=1)


def _draw_mannequin(draw: ImageDraw.ImageDraw, prim: dict, dpi: int) -> None:
    import math

    pos = prim.get("pos") or [100, 160, 0]
    cx, cy = mm_to_px(float(pos[0]), dpi), mm_to_px(float(pos[1]), dpi)
    color = (90, 90, 140)
    joints = prim.get("joints") or {}
    head_r = mm_to_px(8, dpi)
    draw.ellipse((cx - head_r, cy - mm_to_px(42, dpi) - head_r, cx + head_r, cy - mm_to_px(42, dpi) + head_r), outline=color, width=3)
    draw.line((cx, cy - mm_to_px(34, dpi), cx, cy), fill=color, width=3)
    l_yaw = float(joints.get("l_arm", {}).get("yaw", 0.4))
    r_yaw = float(joints.get("r_arm", {}).get("yaw", -0.4))
    arm = mm_to_px(18, dpi)
    ay = cy - mm_to_px(20, dpi)
    l_wx, l_wy = int(cx - arm * math.cos(l_yaw)), int(ay + arm * math.sin(l_yaw))
    r_wx, r_wy = int(cx + arm * math.cos(abs(r_yaw))), int(ay + arm * math.sin(abs(r_yaw)))
    draw.line((cx, ay, l_wx, l_wy), fill=color, width=3)
    draw.line((cx, ay, r_wx, r_wy), fill=color, width=3)
    hand = mm_to_px(10, dpi)
    lw = float(joints.get("l_wrist", {}).get("yaw", 0.0))
    rw = float(joints.get("r_wrist", {}).get("yaw", 0.0))
    draw.line((l_wx, l_wy, int(l_wx - hand * math.cos(l_yaw + lw)), int(l_wy + hand * math.sin(l_yaw + lw))), fill=color, width=2)
    draw.line((r_wx, r_wy, int(r_wx + hand * math.cos(abs(r_yaw) + rw)), int(r_wy + hand * math.sin(abs(r_yaw) + rw))), fill=color, width=2)
    l_leg = float(joints.get("l_leg", {}).get("yaw", 0.15))
    r_leg = float(joints.get("r_leg", {}).get("yaw", -0.15))
    leg = mm_to_px(28, dpi)
    l_ax, l_ay = int(cx - leg * math.sin(l_leg)), cy + leg
    r_ax, r_ay = int(cx + leg * math.sin(abs(r_leg))), cy + leg
    draw.line((cx, cy, l_ax, l_ay), fill=color, width=3)
    draw.line((cx, cy, r_ax, r_ay), fill=color, width=3)
    foot = mm_to_px(8, dpi)
    la = float(joints.get("l_ankle", {}).get("yaw", 0.0))
    ra = float(joints.get("r_ankle", {}).get("yaw", 0.0))
    draw.line((l_ax, l_ay, int(l_ax - foot * math.cos(la)), l_ay + foot // 3), fill=color, width=2)
    draw.line((r_ax, r_ay, int(r_ax + foot * math.cos(ra)), r_ay + foot // 3), fill=color, width=2)


def _balloon_font(line: StoryLine, dpi: int, font_path: str | None) -> ImageFont.ImageFont:
    n = max(1, len(line.text or " "))
    w = mm_to_px(line.w_mm or 40, dpi)
    h = mm_to_px(line.h_mm or 20, dpi)
    cap = mm_to_px(5.0, dpi)
    kind = line.balloon or "speech"
    if getattr(line, "wrap", "horizontal") == "vertical":
        if kind == "none":
            size = max(12, min(w, max(12, h // n)))
        else:
            size = max(12, min(cap, (w * 2) // 3))
    else:
        size = max(12, min(cap, h // 2))
    return _font(font_path, size)


def _draw_balloon(draw: ImageDraw.ImageDraw, line: StoryLine, dpi: int, font_path: str | None = None) -> None:
    x = mm_to_px(line.x_mm, dpi)
    y = mm_to_px(line.y_mm, dpi)
    w = mm_to_px(line.w_mm or 40, dpi)
    h = mm_to_px(line.h_mm or 20, dpi)
    box = [x, y, x + w, y + h]
    kind = line.balloon or "speech"
    font = _balloon_font(line, dpi, font_path)
    size = int(getattr(font, "size", 14) or 14)
    wrap = getattr(line, "wrap", "horizontal")
    page = getattr(draw, "_image", None)
    if wrap == "vertical":
        em = size
        composed = compose_tategaki(
            line.text,
            font,
            em,
            max(em, h if kind == "none" else h),
            fill=(10, 10, 10),
            ruby_runs=getattr(line, "ruby_runs", None) or None,
        )
        pad = 0 if kind == "none" else max(2, em // 4)
        if kind != "none":
            bw = composed.width + pad * 2
            bh = composed.height + pad * 2
            box = [x, y, x + bw, y + bh]
            fill = (255, 255, 255)
            outline = (20, 20, 20)
            if line.path:
                xy = [_xy(pt, dpi) for pt in line.path]
                if len(xy) >= 3:
                    draw.polygon(xy, fill=fill, outline=outline)
            elif kind == "narration":
                draw.rectangle(box, fill=fill, outline=outline, width=2)
            elif kind == "thought":
                draw.ellipse(box, fill=fill, outline=outline, width=2)
                r = max(3, bw // 12)
                draw.ellipse([x + 4, y + bh, x + 4 + r, y + bh + r], fill=fill, outline=outline, width=2)
            else:
                draw.ellipse(box, fill=fill, outline=outline, width=2)
            if line.tail:
                tx, ty = _xy(line.tail, dpi)
                cx = x + bw // 2
                cy = y + bh
                draw.polygon([(cx - 6, cy - 2), (cx + 6, cy - 2), (tx, ty)], fill=fill, outline=outline)
            if line.speaker:
                draw.text((x, max(0, y - size - 2)), line.speaker, fill=(80, 80, 80), font=font)
        if page is not None:
            page.paste(composed, (x + pad, y + pad), composed)
        return
    if line.path:
        xy = [_xy(pt, dpi) for pt in line.path]
        if len(xy) >= 3:
            draw.polygon(xy, fill=(255, 255, 255), outline=(20, 20, 20))
    elif kind != "none":
        fill = (255, 255, 255)
        outline = (20, 20, 20)
        if kind == "narration":
            draw.rectangle(box, fill=fill, outline=outline, width=2)
        elif kind == "thought":
            draw.ellipse(box, fill=fill, outline=outline, width=2)
            r = max(3, w // 12)
            draw.ellipse([x + 4, y + h, x + 4 + r, y + h + r], fill=fill, outline=outline, width=2)
        else:
            draw.ellipse(box, fill=fill, outline=outline, width=2)
        if line.tail:
            tx, ty = _xy(line.tail, dpi)
            cx = x + w // 2
            cy = y + h
            draw.polygon([(cx - 6, cy - 2), (cx + 6, cy - 2), (tx, ty)], fill=fill, outline=outline)
    if line.speaker and kind != "none":
        draw.text((x, max(0, y - size - 2)), line.speaker, fill=(80, 80, 80), font=font)
    pad_x, pad_y = 6, max(2, h // 8)
    if line.ruby:
        draw.text((x + pad_x, y + 2), line.ruby, fill=(10, 10, 10), font=font)
        draw.text((x + pad_x, y + 2 + size), line.text, fill=(10, 10, 10), font=font)
        return
    max_w = max(8, w - pad_x * 2)
    row = ""
    rows: list[str] = []
    for char in line.text:
        trial = row + char
        try:
            bbox = draw.textbbox((0, 0), trial, font=font)
            tw = bbox[2] - bbox[0]
        except Exception:
            tw = len(trial) * size
        if tw <= max_w or not row:
            row = trial
        else:
            rows.append(row)
            row = char
    if row:
        rows.append(row)
    for i, row_text in enumerate(rows):
        draw.text((x + pad_x, y + pad_y + i * (size + 2)), row_text, fill=(10, 10, 10), font=font)


def _draw_crop_marks(draw: ImageDraw.ImageDraw, page: Page, dpi: int) -> None:
    w = mm_to_px(page.spec.width_mm, dpi)
    h = mm_to_px(page.spec.height_mm, dpi)
    bleed = mm_to_px(page.spec.bleed_mm, dpi)
    mark = mm_to_px(5, dpi)
    for x, y, dx, dy in (
        (bleed, bleed, -1, 0),
        (bleed, bleed, 0, -1),
        (w - bleed, bleed, 1, 0),
        (w - bleed, bleed, 0, -1),
        (bleed, h - bleed, -1, 0),
        (bleed, h - bleed, 0, 1),
        (w - bleed, h - bleed, 1, 0),
        (w - bleed, h - bleed, 0, 1),
    ):
        draw.line((x, y, x + dx * mark, y + dy * mark), fill=(0, 0, 0), width=1)


def render_page(
    page: Page,
    working_dpi: int,
    mode: str = "print",
    episode: Episode | None = None,
    crop_marks: bool = False,
    onion: bool = True,
) -> Image.Image:
    width = mm_to_px(page.spec.width_mm, working_dpi)
    height = mm_to_px(page.spec.height_mm, working_dpi)
    size = (width, height)
    image = Image.new("RGB", size, (255, 255, 255))
    include_name = mode in ("name", "proof")

    fill_roles = (LayerRole.BG, LayerRole.INK, LayerRole.FINISH)
    if include_name:
        fill_roles = (LayerRole.BG, LayerRole.NAME, LayerRole.INK, LayerRole.FINISH)
    for role in fill_roles:
        fill = page.fills.get(role)
        if fill is None:
            continue
        if role in (LayerRole.NAME, LayerRole.DRAFT) and mode == "print":
            continue
        image.paste(Image.new("RGB", size, fill), (0, 0))

    rgba = image.convert("RGBA")
    prev_alpha = None
    for layer in page.layers:
        if getattr(layer, "kind", None) == LayerKind.FOLDER:
            continue
        if not layer.visible:
            continue
        if mode == "print" and not layer.exportable:
            continue
        if layer.role in (LayerRole.NAME, LayerRole.DRAFT) and mode == "print":
            continue
        raster = _open_raster(layer)
        if raster is None:
            continue
        raster = raster.resize(size)
        clip_mask = prev_alpha if getattr(layer, "clip", False) else None
        rgba = _blend_over(rgba, raster, getattr(layer, "blend", "normal") or "normal", float(getattr(layer, "opacity", 1.0) or 1.0), clip_mask)
        prev_alpha = raster.split()[3]
    image = rgba.convert("RGB")

    ink_layer = Image.new("RGBA", size, (0, 0, 0, 0))
    name_layer = Image.new("RGBA", size, (0, 0, 0, 0))
    _stroke_draw = ImageDraw.Draw(ink_layer)
    _name_draw = ImageDraw.Draw(name_layer)
    ink_has_raster = any(layer.role == LayerRole.INK and layer.raster_png for layer in page.layers)
    if not (mode == "print" and ink_has_raster):
        for stroke in page.ink_strokes:
            _stroke(_stroke_draw, stroke, working_dpi, INK_COLOR, 3)
    if include_name:
        for stroke in page.name_strokes:
            _stroke(_name_draw, stroke, working_dpi, NAME_COLOR, 3)

    mask = _clip_mask(page, size, working_dpi)
    if mask is not None:
        ink_layer.putalpha(_and_alpha(ink_layer, mask))
        if include_name:
            name_layer.putalpha(_and_alpha(name_layer, mask))

    rgba = image.convert("RGBA")
    if include_name:
        rgba = Image.alpha_composite(rgba, name_layer)
    rgba = Image.alpha_composite(rgba, ink_layer)
    image = rgba.convert("RGB")

    _draw_tone(image, page, working_dpi)
    if page.effects:
        _draw_effects(image, page, working_dpi)
    _draw_prims(image, page, working_dpi, mode)

    if page.ruler and mode in ("name", "proof"):
        draw = ImageDraw.Draw(image)
        for pt in page.ruler.get("points") or []:
            px, py = _xy(pt, working_dpi)
            draw.ellipse((px - 3, py - 3, px + 3, py + 3), outline=(180, 80, 80), width=2)
            draw.line((px, 0, px, image.height), fill=(220, 180, 180), width=1)
            draw.line((0, py, image.width, py), fill=(220, 180, 180), width=1)

    draw = ImageDraw.Draw(image)
    for frame in page.leaf_frames():
        width_px = max(1, mm_to_px(frame.border_mm or 0.8, working_dpi) // 4)
        draw.rectangle(rect_px(frame.rect, working_dpi), outline=(20, 20, 20), width=max(1, width_px))

    font_path = getattr(episode, "font_path", None) if episode is not None else None
    font = _font(font_path)
    lines = episode.story_for_page(page.index) if episode is not None else page.texts
    for line in lines:
        if line.x_mm or line.y_mm or line.balloon:
            _draw_balloon(draw, line, working_dpi, font_path)
        else:
            x = mm_to_px(page.inner_rect_mm().x + 4, working_dpi)
            y = mm_to_px(page.inner_rect_mm().y + 4, working_dpi)
            label = f"{line.speaker}: {line.text}" if line.speaker else line.text
            draw.text((x, y), label, fill=(10, 10, 10), font=font)

    if page.numero and mode == "print":
        label = str(page.index)
        ty = height - mm_to_px(12, working_dpi)
        try:
            bbox = draw.textbbox((0, 0), label, font=font)
            tw = bbox[2] - bbox[0]
        except Exception:
            tw = 6
        draw.text(((width - tw) / 2, ty), label, fill=(20, 20, 20), font=font)

    if crop_marks and mode == "print":
        _draw_crop_marks(draw, page, working_dpi)
    if (
        onion
        and episode is not None
        and mode in ("name", "proof")
        and getattr(page, "onion_from", None)
    ):
        prev = next((item for item in episode.pages if item.index == page.onion_from), None)
        if prev is not None:
            ghost = render_page(prev, working_dpi, mode="print", episode=episode, onion=False)
            ghost = ghost.convert("RGBA")
            r, g, b, a = ghost.split()
            tint = Image.merge("RGBA", (r.point(lambda p: int(p * 0.4)), g.point(lambda p: int(p * 0.4)), b, a.point(lambda p: 70)))
            image = Image.alpha_composite(image.convert("RGBA"), tint).convert("RGB")
    return image


def to_bitonal(image: Image.Image, threshold: int = 180) -> Image.Image:
    return image.convert("L").point(lambda p: 255 if p > threshold else 0, mode="1")


def render_spread(episode: Episode, first: int, second: int, dpi: int = 150, mode: str = "print") -> Image.Image:
    pages = {page.index: page for page in episode.pages}
    a, b = pages[first], pages[second]
    if a.is_recto() and not b.is_recto():
        right, left = a, b
    elif b.is_recto() and not a.is_recto():
        right, left = b, a
    else:
        left, right = a, b
    left_img = render_page(left, dpi, mode=mode, episode=episode)
    right_img = render_page(right, dpi, mode=mode, episode=episode)
    image = Image.new("RGB", (left_img.width + right_img.width, max(left_img.height, right_img.height)), (255, 255, 255))
    image.paste(left_img, (0, 0))
    image.paste(right_img, (left_img.width, 0))
    return image
