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


def _placed_raster(layer, page: Page, episode: Episode | None, size: tuple[int, int], dpi: int,
                   mode: str = "print", finish: bool = True) -> Image.Image | None:
    """Resample a placed image from its asset into its placement, clipped to its panel, and give
    monochrome pages their finish (print: black and white with dots; proof: the flat grey steps)."""
    from genko.assets import AssetStore
    from genko.placement import clip_box

    if episode is None or episode.asset_dir is None or not layer.asset or layer.placement_mm is None:
        return None
    data = AssetStore(episode.asset_dir).get_bytes(layer.asset, ".png")
    if data is None:
        return None
    r = layer.placement_mm  # may start left of / above the paper, so no clamping here
    x0, y0 = round(r.x / 25.4 * dpi), round(r.y / 25.4 * dpi)
    x1, y1 = round((r.x + r.width) / 25.4 * dpi), round((r.y + r.height) / 25.4 * dpi)
    if x1 <= x0 or y1 <= y0:
        return None
    frame = None
    if layer.frame_id:
        try:
            frame = page._find(layer.frame_id)
        except (KeyError, IndexError):
            frame = None
    cx0, cy0, cx1, cy1 = rect_px(clip_box(page, frame, layer.clip_to), dpi)
    # only the visible part is resampled, straight from the source pixels
    vx0, vy0 = max(x0, cx0, 0), max(y0, cy0, 0)
    vx1, vy1 = min(x1, cx1, size[0]), min(y1, cy1, size[1])
    if vx1 <= vx0 or vy1 <= vy0:
        return None
    source = Image.open(io.BytesIO(data)).convert("RGBA")
    sx, sy = source.width / (x1 - x0), source.height / (y1 - y0)
    box = ((vx0 - x0) * sx, (vy0 - y0) * sy, (vx1 - x0) * sx, (vy1 - y0) * sy)
    fitted = source.resize((vx1 - vx0, vy1 - vy0), Image.Resampling.LANCZOS, box=box)
    if finish:
        fitted = _finish_placed(fitted, layer, page, episode, dpi, mode, (vx0, vy0))
    canvas = Image.new("RGBA", size, (0, 0, 0, 0))
    canvas.paste(fitted, (vx0, vy0))
    return canvas


def _finish_placed(fitted: Image.Image, layer, page: Page, episode: Episode | None, dpi: int, mode: str,
                   origin: tuple[int, int]) -> Image.Image:
    from genko import screentone

    to = (layer.source or {}).get("to", "art")
    if mode not in ("print", "proof") or page.spec.expression == "color" or layer.role in (LayerRole.DRAFT, LayerRole.NAME):
        return fitted
    alpha = fitted.split()[3]
    grey = Image.alpha_composite(Image.new("RGBA", fitted.size, (255, 255, 255, 255)), fitted).convert("L")
    if to == "ink":
        # extracted line art: pure black lines, transparent elsewhere
        ink = grey.point(lambda v: 255 if v < 128 else 0)
        out = Image.new("RGBA", fitted.size, (0, 0, 0, 0))
        out.putalpha(ImageChops.multiply(ink, alpha))
        return out
    style = ((episode.studio.get("style") or {}).get("finish") if episode is not None else None) or {}
    finish = screentone.Finish.from_dict({**style, **(layer.finish or {})})
    done = screentone.finish_gray(grey, finish, dpi, screen=mode == "print", origin=origin)
    out = done.convert("RGBA")
    out.putalpha(alpha if mode == "proof" else alpha.point(lambda v: 255 if v >= 128 else 0))
    return out


def render_frame(page: Page, frame_id: str, working_dpi: int, mode: str = "proof", episode: Episode | None = None) -> Image.Image:
    """One panel, cropped from the page render (bleed panels include their bleed)."""
    from genko.placement import clip_box

    frame = page._find(frame_id)
    image = render_page(page, working_dpi, mode=mode, episode=episode)
    x0, y0, x1, y1 = rect_px(clip_box(page, frame, "bleed" if frame.bleed else "frame"), working_dpi)
    return image.crop((max(0, x0), max(0, y0), min(image.width, x1), min(image.height, y1)))


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


def _tone_is_fm(layer) -> bool:
    if not layer.material_id:
        return False
    try:
        from genko.materials import get_material

        return get_material(str(layer.material_id)).get("kind") == "noise"
    except Exception:
        return False


def _draw_tone(image: Image.Image, page: Page, dpi: int, mode: str = "print", finish: bool = True) -> Image.Image:
    """Tone layers: AM dots (or FM for noise materials) with the exact black share, inside their
    region (or all panels). Proofs and names show a flat grey instead of dots."""
    from genko import screentone

    for layer in page.layers:
        if layer.role != LayerRole.TONE or not layer.visible:
            continue
        density = max(0.0, min(1.0, float(layer.density or 0.3)))
        mask = Image.new("L", image.size, 0)
        draw = ImageDraw.Draw(mask)
        if layer.region:
            draw.polygon([_xy(pt, dpi) for pt in layer.region], fill=255)
        else:
            for frame in page.leaf_frames():
                draw.rectangle(rect_px(frame.rect, dpi), fill=255)
        if mode == "print" and finish:
            tone = screentone.tone_area(mask, density, dpi, float(layer.lpi or 60), float(layer.angle if layer.angle is not None else 45),
                                        fm=_tone_is_fm(layer))
        else:
            tone = Image.new("RGBA", image.size, (0, 0, 0, 0))
            tone.putalpha(mask.point(lambda v, d=density: round(v * d)))
        image = Image.alpha_composite(image.convert("RGBA"), tone).convert(image.mode)
    return image


def _draw_effects(image: Image.Image, page: Page, dpi: int) -> Image.Image:
    """Focus lines (tapered wedges toward a clear centre), speed lines and white flash, each clipped
    to its panel. Lengths vary with a fixed per-effect sequence, so renders repeat exactly."""
    import math
    import random

    for effect in page.effects:
        kind = effect.get("kind")
        frame = None
        if effect.get("frame_id"):
            try:
                frame = page._find(effect["frame_id"])
            except (KeyError, IndexError):
                frame = None
        box = rect_px(frame.rect, dpi) if frame is not None else (0, 0, image.width, image.height)
        x0, y0, x1, y1 = box
        params = effect.get("params") or {}
        count = int(params.get("count", 48 if kind == "focus" else 28))
        rng = random.Random(str(effect.get("id")))
        layer = Image.new("RGBA", image.size, (0, 0, 0, 0))
        draw = ImageDraw.Draw(layer)
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        if kind == "focus":
            outer = math.hypot(x1 - x0, y1 - y0) / 2
            clear = float(params.get("clear", 0.45))  # the empty centre, as a share of the half-diagonal
            for i in range(count):
                a = 2 * math.pi * (i + rng.random() * 0.6) / count
                inner = outer * clear * (0.85 + rng.random() * 0.3)
                half = (2 * math.pi / count) * (0.18 + rng.random() * 0.2)
                tip = (cx + inner * math.cos(a), cy + inner * math.sin(a))
                draw.polygon([(cx + outer * 1.05 * math.cos(a - half), cy + outer * 1.05 * math.sin(a - half)),
                              (cx + outer * 1.05 * math.cos(a + half), cy + outer * 1.05 * math.sin(a + half)), tip],
                             fill=(15, 15, 15, 255))
        elif kind == "speed":
            height = max(1, y1 - y0)
            thick = max(1, mm_to_px(0.3, dpi))
            for i in range(count):
                y = y0 + height * (i + rng.random()) / count
                start = x0 + (x1 - x0) * rng.random() * 0.5
                draw.line((start, y, x1, y), fill=(15, 15, 15, 255), width=thick + int(rng.random() * thick * 2))
        elif kind == "white":
            draw.rectangle(box, fill=(255, 255, 255, 255))
        else:
            continue
        if frame is not None:
            mask = Image.new("L", image.size, 0)
            ImageDraw.Draw(mask).rectangle(box, fill=255)
            layer.putalpha(ImageChops.multiply(layer.split()[3], mask))
        image = Image.alpha_composite(image.convert("RGBA"), layer).convert(image.mode)
    return image


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


def _draw_mannequin(draw: ImageDraw.ImageDraw, prim: dict, dpi: int, color=(90, 90, 140)) -> None:
    """The posable figure (genko.mannequin): body, elbows, knees and neck, turned and leaned by rot."""
    from genko import mannequin

    bone = mannequin.skeleton(prim)
    width = max(2, mm_to_px(0.5, dpi))
    shade = {"body": color, "left": (60, 120, 170), "right": (150, 90, 120)}
    for a, b, part in bone["segments"]:
        draw.line([_xy(a, dpi), _xy(b, dpi)], fill=shade.get(part, color), width=width)
    for a, _, part in bone["segments"]:
        if part != "body":
            px, py = _xy(a, dpi)
            r = max(1, width)
            draw.ellipse((px - r, py - r, px + r, py + r), fill=shade[part])
    (hx, hy), hr = bone["head"]
    x0, y0 = _xy((hx - hr, hy - hr), dpi)
    x1, y1 = _xy((hx + hr, hy + hr), dpi)
    draw.ellipse((x0, y0, x1, y1), outline=color, width=width)
    nose = (hx + hr * 0.8 * bone["facing"], hy)
    draw.line([_xy((hx, hy), dpi), _xy(nose, dpi)], fill=color, width=max(1, width // 2))


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


SQRT2 = 2 ** 0.5
OUTLINE = (20, 20, 20)
PAPER = (255, 255, 255)


def _ellipse_point(cx: float, cy: float, rx: float, ry: float, t: float) -> tuple[float, float]:
    import math

    return cx + rx * math.cos(t), cy + ry * math.sin(t)


def _tail(draw: ImageDraw.ImageDraw, cx: float, cy: float, rx: float, ry: float, target: tuple[int, int],
          width: int, rect: bool = False) -> None:
    """A tail from the balloon's edge toward `target`, drawn after the balloon. The base is a third of
    the short side (measured as a chord, so tall balloons get wide tails too) and leaves from whichever
    side faces the speaker; the outline is opened where the tail joins."""
    import math

    tx, ty = target
    if abs(tx - cx) < 1e-6 and abs(ty - cy) < 1e-6:
        return
    base = max(min(rx, ry) * 2 / 3, 4.0)
    t = math.atan2((ty - cy) / max(ry, 1), (tx - cx) / max(rx, 1))
    if rect:
        if abs(tx - cx) / max(rx, 1) > abs(ty - cy) / max(ry, 1):
            ex = cx + (rx if tx > cx else -rx)
            p1, p2 = (ex, cy - base / 2), (ex, cy + base / 2)
        else:
            ey = cy + (ry if ty > cy else -ry)
            p1, p2 = (cx - base / 2, ey), (cx + base / 2, ey)
    else:
        lo, hi = 0.0, math.pi / 2
        for _ in range(24):  # the half-angle whose chord is `base`
            mid = (lo + hi) / 2
            a = _ellipse_point(cx, cy, rx, ry, t - mid)
            b = _ellipse_point(cx, cy, rx, ry, t + mid)
            if math.dist(a, b) < base:
                lo = mid
            else:
                hi = mid
        p1 = _ellipse_point(cx, cy, rx, ry, t - lo)
        p2 = _ellipse_point(cx, cy, rx, ry, t + lo)
    # the triangle reaches a little inside the balloon so the white covers the outline at the join
    k = 0.12
    q1 = (p1[0] + (cx - p1[0]) * k, p1[1] + (cy - p1[1]) * k)
    q2 = (p2[0] + (cx - p2[0]) * k, p2[1] + (cy - p2[1]) * k)
    draw.polygon([q1, q2, (tx, ty)], fill=PAPER)
    draw.line([p1, (tx, ty), p2], fill=OUTLINE, width=width, joint="curve")


def _balloon_shape(draw: ImageDraw.ImageDraw, kind: str, cx: float, cy: float, rx: float, ry: float,
                   tail: tuple[int, int] | None, width: int) -> None:
    import math

    box = [cx - rx, cy - ry, cx + rx, cy + ry]
    if kind == "narration":
        draw.rectangle(box, fill=PAPER, outline=OUTLINE, width=width)
        return
    if kind == "shout":
        spikes = max(12, int((rx + ry) / 6))
        points = []
        for i in range(spikes * 2):
            t = math.pi * i / spikes
            k = 1.18 if i % 2 == 0 else 0.98
            points.append(_ellipse_point(cx, cy, rx * k, ry * k, t))
        draw.polygon(points, fill=PAPER)
        draw.line(points + points[:1], fill=OUTLINE, width=width)
        if tail:
            _tail(draw, cx, cy, rx, ry, tail, width)
        return
    if kind == "whisper":
        draw.ellipse(box, fill=PAPER)
        dashes = max(16, int((rx + ry) / 4))
        for i in range(0, dashes * 2, 2):
            t0, t1 = math.pi * i / dashes, math.pi * (i + 1) / dashes
            draw.line([_ellipse_point(cx, cy, rx, ry, t0 + (t1 - t0) * k / 4) for k in range(5)], fill=OUTLINE, width=width)
        if tail:
            _tail(draw, cx, cy, rx, ry, tail, width)
        return
    draw.ellipse(box, fill=PAPER, outline=OUTLINE, width=width)
    if tail and kind == "speech":
        _tail(draw, cx, cy, rx, ry, tail, width)
    if kind == "thought":
        # small bubbles toward the speaker (or down-left when no tail is set)
        tx, ty = tail if tail else (cx - rx * 1.6, cy + ry * 1.4)
        ang = math.atan2((ty - cy) / max(ry, 1), (tx - cx) / max(rx, 1))
        ex, ey = _ellipse_point(cx, cy, rx, ry, ang)
        for i, frac in enumerate((0.25, 0.55, 0.85)):
            r = max(2.0, min(rx, ry) * (0.22 - i * 0.06))
            bx, by = ex + (tx - ex) * frac, ey + (ty - ey) * frac
            draw.ellipse([bx - r, by - r, bx + r, by + r], fill=PAPER, outline=OUTLINE, width=max(1, width - 1))


def _outlined(text_img: Image.Image, grow: int) -> Image.Image:
    """Black text with a white outline `grow` px wide (for SFX over art)."""
    from PIL import ImageFilter

    alpha = text_img.split()[3]
    halo = alpha.filter(ImageFilter.MaxFilter(grow * 2 + 1)) if grow > 0 else alpha
    out = Image.new("RGBA", text_img.size, (255, 255, 255, 0))
    out.putalpha(halo)
    out.alpha_composite(text_img)
    return out


def _draw_balloon(
    draw: ImageDraw.ImageDraw, line: StoryLine, dpi: int, font_path: str | None = None, show_speaker: bool = True
) -> None:
    x = mm_to_px(line.x_mm, dpi)
    y = mm_to_px(line.y_mm, dpi)
    w = mm_to_px(line.w_mm or 40, dpi)
    h = mm_to_px(line.h_mm or 20, dpi)
    kind = line.balloon or "speech"
    font = _balloon_font(line, dpi, font_path)
    size = int(getattr(font, "size", 14) or 14)
    wrap = getattr(line, "wrap", "horizontal")
    page = getattr(draw, "_image", None)
    width = max(2, mm_to_px(0.35, dpi))
    tail = _xy(line.tail, dpi) if line.tail else None
    cx, cy = x + w / 2, y + h / 2
    if wrap == "vertical":
        em = size
        pad = 0 if kind in ("none", "sfx") else max(2, em // 4)
        if kind == "sfx":
            em = max(12, min(mm_to_px(12, dpi), w, h // max(1, len((line.text or " ").split("\n")[0]))))
            font = _font(font_path, em)
        composed = compose_tategaki(
            line.text,
            font,
            em,
            max(em, h),
            fill=(10, 10, 10),
            ruby_runs=getattr(line, "ruby_runs", None) or None,
        )
        tw, th = composed.size
        if kind == "none":
            if page is not None:
                page.paste(composed, (x, y), composed)
            return
        if kind == "sfx":
            if page is not None:
                art = _outlined(composed, max(2, em // 8))
                page.paste(art, (round(cx - art.width / 2), round(cy - art.height / 2)), art)
            return
        if kind == "narration":
            rx, ry = tw / 2 + pad, th / 2 + pad
        else:
            # the ellipse goes around the text block's corners: half-size × √2, plus the pad
            rx, ry = tw / 2 * SQRT2 + pad, th / 2 * SQRT2 + pad
        if line.path:
            xy = [_xy(pt, dpi) for pt in line.path]
            if len(xy) >= 3:
                draw.polygon(xy, fill=PAPER, outline=OUTLINE)
        else:
            _balloon_shape(draw, kind, cx, cy, rx, ry, tail if kind != "narration" else None, width)
        if line.speaker and show_speaker:
            draw.text((min(x, cx - rx), max(0, min(y, cy - ry) - size - 2)), line.speaker, fill=(80, 80, 80), font=font)
        if page is not None:
            page.paste(composed, (round(cx - tw / 2), round(cy - th / 2)), composed)
        return
    if line.path:
        xy = [_xy(pt, dpi) for pt in line.path]
        if len(xy) >= 3:
            draw.polygon(xy, fill=PAPER, outline=OUTLINE)
    elif kind not in ("none", "sfx"):
        _balloon_shape(draw, kind, cx, cy, w / 2, h / 2, tail if kind != "narration" else None, width)
    if line.speaker and kind != "none" and show_speaker:
        draw.text((x, max(0, y - size - 2)), line.speaker, fill=(80, 80, 80), font=font)
    pad_x, pad_y = 6, max(2, h // 8)
    if kind in ("speech", "thought", "shout", "whisper"):
        # keep the text inside the ellipse: the inscribed box is the ellipse's size / √2
        pad_x = max(pad_x, round(w / 2 * (1 - 1 / SQRT2)))
        pad_y = max(pad_y, round(h / 2 * (1 - 1 / SQRT2)))
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


def _draw_frames(draw: ImageDraw.ImageDraw, page: Page, working_dpi: int) -> None:
    for frame in page.leaf_frames():
        width_px = max(1, mm_to_px(frame.border_mm or 0.8, working_dpi) // 4)
        if not frame.bleed:
            draw.rectangle(rect_px(frame.rect, working_dpi), outline=(20, 20, 20), width=max(1, width_px))
            continue
        # A bleed panel has no border on the sides that run off the paper.
        from genko.placement import outer_edges

        x0, y0, x1, y1 = rect_px(frame.rect, working_dpi)
        open_sides = outer_edges(page, frame)
        for side, line in (("top", (x0, y0, x1, y0)), ("bottom", (x0, y1, x1, y1)), ("left", (x0, y0, x0, y1)), ("right", (x1, y0, x1, y1))):
            if not open_sides[side]:
                draw.line(line, fill=(20, 20, 20), width=max(1, width_px))


def render_page(
    page: Page,
    working_dpi: int,
    mode: str = "print",
    episode: Episode | None = None,
    crop_marks: bool = False,
    onion: bool = True,
    finish: bool | None = None,
) -> Image.Image:
    """`finish`: the monochrome print finish (dots and pure black and white). None = by the page
    (mono pages yes, colour pages no); False for screen and colour outputs (webtoon, SNS)."""
    if finish is None:
        finish = page.spec.expression != "color"
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
        if layer.kind == LayerKind.PLACED:
            raster = _placed_raster(layer, page, episode, size, working_dpi, mode, finish)
        else:
            raster = _open_raster(layer)
            if raster is not None:
                raster = raster.resize(size)
        if raster is None:
            continue
        clip_mask = prev_alpha if getattr(layer, "clip", False) else None
        opacity = getattr(layer, "opacity", None)
        opacity = 1.0 if opacity is None else float(opacity)  # 0 means invisible, not "default"
        rgba = _blend_over(rgba, raster, getattr(layer, "blend", "normal") or "normal", opacity, clip_mask)
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

    image = _draw_tone(image, page, working_dpi, mode, finish)
    if page.effects:
        image = _draw_effects(image, page, working_dpi)
    _draw_prims(image, page, working_dpi, mode)

    if page.ruler and mode in ("name", "proof"):
        draw = ImageDraw.Draw(image)
        for pt in page.ruler.get("points") or []:
            px, py = _xy(pt, working_dpi)
            draw.ellipse((px - 3, py - 3, px + 3, py + 3), outline=(180, 80, 80), width=2)
            draw.line((px, 0, px, image.height), fill=(220, 180, 180), width=1)
            draw.line((0, py, image.width, py), fill=(220, 180, 180), width=1)

    draw = ImageDraw.Draw(image)
    _draw_frames(draw, page, working_dpi)

    font_path = getattr(episode, "font_path", None) if episode is not None else None
    font = _font(font_path)
    lines = episode.story_for_page(page.index) if episode is not None else page.texts
    for line in lines:
        if line.x_mm or line.y_mm or line.balloon:
            # Speaker names are a working aid: shown in name/proof, never printed.
            _draw_balloon(draw, line, working_dpi, font_path, show_speaker=mode != "print")
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


def render_spread(episode: Episode, first: int, second: int, dpi: int = 150, mode: str = "print",
                  finish: bool | None = None) -> Image.Image:
    pages = {page.index: page for page in episode.pages}
    a, b = pages[first], pages[second]
    # Place by the physical side of the book (binding-aware), not by page parity.
    if a.side(episode.start_side) == "right" and b.side(episode.start_side) == "left":
        right, left = a, b
    elif b.side(episode.start_side) == "right" and a.side(episode.start_side) == "left":
        right, left = b, a
    else:
        left, right = a, b
    left_img = render_page(left, dpi, mode=mode, episode=episode, finish=finish)
    right_img = render_page(right, dpi, mode=mode, episode=episode, finish=finish)
    image = Image.new("RGB", (left_img.width + right_img.width, max(left_img.height, right_img.height)), (255, 255, 255))
    image.paste(left_img, (0, 0))
    image.paste(right_img, (left_img.width, 0))
    return image
